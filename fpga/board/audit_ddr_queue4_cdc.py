"""Audit real PDS mapped cell hierarchy and SDC binding annotations.
PDS get_cells returns symbolic handles, not Tcl lists. Read the mapped VM and
its exact board.fdc; bind every reported group to actual cells/constraint IDs.
"""
import argparse
import fnmatch
import json
import re
from pathlib import Path


MODULE = re.compile(r'^[ \t]*module\s+(\w+)\b(.*?)^endmodule\b', re.M | re.S)
INSTANCE = re.compile(r'^\s*(GTP_\w+|ag_hmic_\w+)\s+(?:/\*.*?\*/\s*)?(?:#\s*\(.*?\)\s*)?(\\\S+|[A-Za-z_]\w*)\s*\((.*?)\);', re.M | re.S)
ANNOT = re.compile(r'SDC constraint\s*:\s*\(object ([^)]+)\)\s*\(id (\d+)\)\s*\((?:cell\s+)?(from|to)\)\s*\((\d+)\s*:\s*([^\n]+)\)')


def normalized_path(value):
    value = str(value).replace('\\', '/').strip().lower()
    return re.sub(r'^/mnt/([a-z])/', lambda m: m.group(1) + ':/', value)


def mapped_cells(text):
    # ADS 2025 emits attributes immediately before module and instance names.
    text = re.sub(r'[(][*].*?[*][)]', '', text, flags=re.S)
    modules = {}
    for module in MODULE.finditer(text):
        name, body = module.group(1), module.group(2)
        if name != 'ag_npu_board_top' and not name.startswith('ag_hmic_'):
            continue
        body = body[body.find(');') + 2:]
        matches = list(INSTANCE.finditer(body))
        rows = []
        for i, match in enumerate(matches):
            tail = body[match.end():matches[i + 1].start() if i + 1 < len(matches) else len(body)]
            rows.append({'type': match.group(1), 'name': match.group(2).lstrip('\\'), 'pins': match.group(3),
                         'annotations': [{'pin': a[0], 'id': int(a[1]), 'direction': a[2], 'line': int(a[3]), 'file': a[4]} for a in ANNOT.findall(tail)]})
        modules[name] = rows
    if 'ag_npu_board_top' not in modules:
        raise ValueError('mapped top ag_npu_board_top missing')
    cells = []
    def visit(module, prefix, parent_nets=None):
        parent_nets = parent_nets or {}
        def resolve(net):return parent_nets.get(net.strip(), (prefix+'/' if prefix else '')+net.strip())
        def connections(pins):return {m.group(1):resolve(m.group(2)) for m in re.finditer(r'\.(\w+)\s*\(([^()]+)\)',pins)}
        for row in modules[module]:
            path = prefix + '/' + row['name'] if prefix else row['name']
            if row['type'] in modules:
                visit(row['type'], path, connections(row['pins']))
            elif row['type'].startswith('GTP_'):
                cells.append(dict(row, path=path, resolved_pins=connections(row['pins'])))
    visit('ag_npu_board_top', '')
    if not any(c['path'].startswith('bridge/') for c in cells):
        raise ValueError('real mapped bridge hierarchy missing')
    return cells


def cell_query(expression, cells):
    match = re.fullmatch(r'\[get_cells\s+([^{}]*)\{([^}]+)\}\]', expression.strip())
    if not match:
        raise ValueError('unsupported cell query ' + expression)
    options, pattern = match.groups()
    if '-regexp' in options:
        return [c for c in cells if re.fullmatch(pattern, c['path'])]
    if re.search(r'(?:wgray|rgray)(?:_r1|_w1)?$', pattern):
        # FDC is evaluated on RTL bus registers; mapped VM scalarizes them.
        # Strip ONLY the three valid bit suffixes, never sync-stage suffixes.
        return [c for c in cells if fnmatch.fnmatchcase(re.sub(r'\[[0-9]+\]$', '', c['path']), pattern)]
    return [c for c in cells if fnmatch.fnmatchcase(c['path'], pattern)]


def bindings(rows, direction, line, fdc):
    result = None
    for cell in rows:
        ids = {a['id'] for a in cell['annotations'] if a['direction'] == direction and a['line'] == line and normalized_path(a['file']) == normalized_path(fdc.resolve())}
        if not ids:
            raise ValueError(f"unbound {direction} line{line} real cell {cell['path']}")
        result = ids if result is None else result & ids
    if not result:
        raise ValueError('no common applied constraint id')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--netlist', required=True, type=Path)
    parser.add_argument('--fdc', required=True, type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--depth', type=int, choices=(4,8,16,32), default=4)
    parser.add_argument('--require-precise', action='store_true')
    parser.add_argument('--require-bounded-gray', action='store_true')
    args = parser.parse_args()
    pointer_bits = args.depth.bit_length()
    expected_bits = {str(bit) for bit in range(pointer_bits)}
    text = args.netlist.read_text(errors='replace')
    fdc = args.fdc.read_text(errors='replace')
    # Derive CPU source period from the frozen generated-clock constraint,
    # then bind it to PDS's requested clock frequency after synthesis.
    board_clock = re.search(r'^create_clock -name board_27m -period ([0-9.]+)', fdc, re.M)
    generated = re.search(r'^create_generated_clock -name cpu_clk .*?-multiply_by ([0-9]+) -divide_by ([0-9]+)', fdc, re.M)
    direct = re.search(r'^create_clock -name cpu_clk -period ([0-9.]+)', fdc, re.M)
    if board_clock and generated:
        cpu_period_ns = float(board_clock.group(1)) * int(generated.group(2)) / int(generated.group(1))
    elif direct:
        cpu_period_ns = float(direct.group(1))
    else:
        raise ValueError('CPU source clock period not constrained')
    timing = args.netlist.with_name(args.netlist.name.replace('_syn.vm', '.snr'))
    if not timing.is_file():raise ValueError('PDS synthesis clock report missing')
    timing_text = timing.read_text(errors='replace')
    cpu_report = re.search(r'^\s*cpu_clk\s+([0-9.]+)\s+MHz', timing_text, re.M)
    ui_report = re.search(r'^\s*ddrphy_sysclk\s+([0-9.]+)\s+MHz', timing_text, re.M)
    if not cpu_report or not ui_report:raise ValueError('CPU or UI clock absent from actual PDS frequency table')
    cpu_mhz,ui_mhz = float(cpu_report.group(1)),float(ui_report.group(1))
    if cpu_mhz<=0 or ui_mhz<=0 or abs(1000/cpu_period_ns-cpu_mhz)>0.1:
        raise ValueError('PDS clock frequency differs from frozen FDC CPU period')
    ui_period_ns=1000/ui_mhz
    cells = mapped_cells(text)
    clock_nets={'cpu_clk':'cpu_clk'}
    top_body=next(m.group(2) for m in MODULE.finditer(re.sub(r'[(][*].*?[*][)]','',text,flags=re.S)) if m.group(1)=='ag_npu_board_top')
    ddr_instance=re.search(r'ag_ddr3\s+I_ips_ddr_top\s*\((.*?)\);',top_body,re.S)
    if not ddr_instance:raise ValueError('top-level DDR controller instance missing')
    ui_net=re.search(r'\.core_clk\s*\(([^)]+)\)',ddr_instance.group(1))
    if not ui_net or not re.search(r'create_generated_clock -name \{?ddrphy_sysclk\}? .*I_ips_ddr_top',fdc):raise ValueError('DDR UI generated clock binding missing')
    clock_nets['ddrphy_sysclk']=ui_net.group(1).strip()
    report = {'passed': False, 'netlist': str(args.netlist.resolve()),
              'fdc': str(args.fdc.resolve()), 'depth': args.depth,
              'scope': 'actual PDS mapped-cell and emitted SDC binding audit, not timing closure',
              'cpu_period_ns': cpu_period_ns, 'cpu_mhz': cpu_mhz,
              'ui_period_ns': ui_period_ns, 'groups': []}
    try:
        gray = payload = 0
        for lineno, line in enumerate(fdc.splitlines(), 1):
            if not line.startswith('set_max_delay '):
                continue
            clock_bound=re.fullmatch(r'set_max_delay\s+([0-9.]+)\s+-datapath_only\s+-from\s+\[get_clocks (cpu_clk|ddrphy_sysclk)\]\s+-to\s+\[get_clocks (cpu_clk|ddrphy_sysclk)\]',line)
            if clock_bound:
                limit,source_clock,target_clock=clock_bound.groups()
                if source_clock==target_clock or float(limit)>8:raise ValueError('invalid payload clock bound')
                fifo='requests' if source_clock=='cpu_clk' else 'responses'
                storage=[c for c in cells if '/'+fifo+'/payload' in c['path'] and c['type'].startswith('GTP_RAM')]
                if not storage:raise ValueError('payload storage missing for '+source_clock)
                if any(c['resolved_pins'].get('WCLK')!=clock_nets[source_clock] for c in storage):raise ValueError('payload write clock differs from selected source clock')
                register_clocks={c['resolved_pins'].get('CLK') for c in cells if '/'+fifo+'/' in c['path'] and c['type'].startswith('GTP_DFF')}
                if not {clock_nets[source_clock],clock_nets[target_clock]}.issubset(register_clocks):raise ValueError('FIFO source/destination clock connection missing')
                if re.search(r'SDC-2023|CommandTiming-0057|Parse file error', timing_text):raise ValueError('PDS did not accept the clock-bound constraints')
                report['groups'].append({'line':lineno,'limit_ns':float(limit),'source_clock':source_clock,'target_clock':target_clock,'payload_cells':len(storage),'binding_kind':'clock-to-clock constraint with mapped FIFO clock tracing; per-cell SDC annotations are not emitted'})
                payload+=1
                print('QUEUE_PAYLOAD_CLOCK_BOUND_PASS',source_clock,target_clock,'cells',len(storage))
                continue
            if 'bridge/' not in line:continue
            match = re.match(r'set_max_delay\s+([0-9.]+)\s+-datapath_only\s+-from\s+(\[get_cells.*?\])\s+-to\s+(\[get_(?:cells|clocks).*\])', line)
            if not match:
                raise ValueError('unrecognized constraint ' + line)
            limit, source, target = match.groups()
            src = cell_query(source, cells)
            if not src:
                raise ValueError('EMPTY source ' + source)
            ids = bindings(src, 'from', lineno, args.fdc)
            row = {'line': lineno, 'limit_ns': float(limit), 'source_query': source, 'source_count': len(src),
                   'source_cells': [c['path'] for c in src], 'constraint_ids': sorted(ids)}
            if target.startswith('[get_cells'):
                dst = cell_query(target, cells)
                if not dst:
                    raise ValueError('EMPTY target ' + target)
                targetids = bindings(dst, 'to', lineno, args.fdc)
                if not ids & targetids:
                    raise ValueError('source/target do not share applied SDC id')
                if len(dst) != pointer_bits or any(not c['type'].startswith('GTP_DFF') for c in dst):
                    raise ValueError('Gray first synchronizer must be exactly the expected Gray bus width')
                dst_matches = [re.fullmatch(r'(.*/)(wgray_r1|rgray_w1)\[([0-9]+)\]', c['path']) for c in dst]
                if not all(dst_matches):
                    raise ValueError('unexpected Gray destination names')
                roots = {m.group(1) for m in dst_matches}
                dest_buses = {m.group(2) for m in dst_matches}
                if len(roots) != 1 or len(dest_buses) != 1 or {m.group(3) for m in dst_matches} != expected_bits:
                    raise ValueError('first-sync bus is incomplete or spans FIFOs')
                root = next(iter(roots))
                dest_bus = next(iter(dest_buses))
                source_bus = 'wgray' if dest_bus == 'wgray_r1' else 'rgray'
                sync_suffix = '_r' if source_bus == 'wgray' else '_w'
                true_source = [c for c in src if re.fullmatch(re.escape(root + source_bus) + r'\[[0-9]+\]', c['path'])]
                extras = [c for c in src if c not in true_source]
                true_bits = {re.search(r'\[([0-9]+)\]$', c['path']).group(1) for c in true_source}
                bounded = len(true_source) == pointer_bits and true_bits == expected_bits
                bounded = bounded and all(c['type'].startswith('GTP_DFF') for c in src)
                bounded = bounded and all(re.fullmatch(re.escape(root + source_bus + sync_suffix) + r'[12]\[[0-9]+\]', c['path']) for c in extras)
                # Extra endpoints must use exactly the first-sync clock. They
                # add a conservative bound, never a setup false-path exception.
                first_clocks = {re.search(r'\.CLK\s*\(([^)]+)\)', c['pins']).group(1).strip() for c in dst}
                extra_clocks = {re.search(r'\.CLK\s*\(([^)]+)\)', c['pins']).group(1).strip() for c in extras}
                bounded = bounded and len(first_clocks) == 1 and extra_clocks.issubset(first_clocks)
                precise = bounded and not extras
                if args.require_precise and not precise:
                    raise ValueError('Gray source contains synchronizer stages/extra cells')
                if args.require_bounded_gray and not bounded:
                    raise ValueError('true Gray bus missing or unexpected extra endpoints/clocks')
                # requests.wgray and responses.rgray originate in CPU clock.
                # Reverse pointers originate in UI clock. Bound each Gray bit
                # within one SOURCE period, leaving 0.15ns holdoff margin.
                cpu_source=('requests' in root and source_bus=='wgray') or ('responses' in root and source_bus=='rgray')
                source_period=cpu_period_ns if cpu_source else ui_period_ns
                safe_limit=min(7.0,source_period-0.15)
                if safe_limit<=0 or float(limit)>safe_limit:
                    raise ValueError(f'Gray delay {limit}ns exceeds source period bound {safe_limit:.3f}ns')
                row.update(source_clock='cpu_clk' if cpu_source else 'ddrphy_sysclk',
                           source_period_ns=source_period, safe_limit_ns=safe_limit)
                row.update(target_query=target, target_count=len(dst), target_cells=[c['path'] for c in dst], source_precise=precise,
                           true_gray_source_cells=[c['path'] for c in true_source], extra_source_sync_cells=[c['path'] for c in extras],
                           extra_sync_clock=sorted(extra_clocks), all_true_gray_sources_bounded=bounded)
                gray += 1
            else:
                clock_match = re.fullmatch(r'\[get_clocks\s+\{?(\w+)\}?\]', target)
                if not clock_match:
                    raise ValueError('unrecognized target clock query')
                clock = clock_match.group(1)
                timing = args.netlist.with_name(args.netlist.name.replace('_syn.vm', '.snr'))
                if not timing.is_file():
                    raise ValueError('actual PDS timing report missing for clock target')
                actual_clock = re.search(r'^\s*' + re.escape(clock) + r'\s+([0-9.]+)\s+MHz', timing.read_text(errors='replace'), re.M)
                if not actual_clock:
                    raise ValueError('clock absent from actual PDS frequency table ' + clock)
                if not all('payload' in c['path'] and c['type'].startswith('GTP_RAM') for c in src):
                    raise ValueError('payload group not actual mapped RAM storage')
                if float(limit) > 8:
                    raise ValueError('payload delay exceeds8ns')
                row.update(target_binding_kind='named clock in applied FDC and actual PDS timing report; PDS emits no per-cell TO annotations for clock target',
                           clock_frequency_mhz=float(actual_clock.group(1)), clock_report=str(timing.resolve()),
                           target_query=target, target_count=1, target_cells=['PDS_CLOCK:' + clock])
                payload += 1
            report['groups'].append(row)
            print('QUEUE4_CDC_REAL_MATCH', lineno, 'sources', len(src), 'targets', row['target_count'])
            for cell in src:
                print('QUEUE4_CDC_SOURCE', cell['path'])
            for name in row['target_cells']:
                print('QUEUE4_CDC_TARGET', name)
        if gray != 4 or payload != 2:
            raise ValueError(f'missing required groups gray={gray},payload={payload}')
        for statement in fdc.splitlines():
            if statement.startswith('set_clock_groups') and any(s in statement for s in ('cpu_clk', 'ddrphy_sysclk')):
                raise ValueError('clock groups can mask CDC bounds')
            if statement.startswith('set_false_path') and '-hold' not in statement:
                if any(s in statement for s in ('wgray', 'rgray', 'payload')):
                    raise ValueError('setup false path masks Gray/payload')
                if 'cpu_clk' in statement and 'ddrphy_sysclk' in statement:
                    raise ValueError('whole CPU/UI setup false path masks payload')
        report['all_gray_sources_precise'] = all(r.get('source_precise', True) for r in report['groups'])
        report['all_true_gray_sources_bounded'] = all(r.get('all_true_gray_sources_bounded', True) for r in report['groups'])
        report['audit_mode'] = 'precise' if args.require_precise else 'bounded-gray' if args.require_bounded_gray else 'bindings-only'
        report['passed'] = True
    except BaseException as error:
        report['failure'] = repr(error)
        raise
    finally:
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('QUEUE4_CDC_REAL_BINDINGS_PASS', 'depth', args.depth, 'groups', len(report['groups']))


if __name__ == '__main__':
    main()
