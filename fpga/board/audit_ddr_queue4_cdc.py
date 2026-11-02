"""Audit real PDS mapped cell hierarchy and SDC binding annotations.
PDS get_cells returns symbolic handles, not Tcl lists. Read the mapped VM and
its exact board.fdc; bind every reported group to actual cells/constraint IDs.
"""
import argparse
import fnmatch
import hashlib
import json
import re
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


MODULE = re.compile(r'^module\s+(\w+)\b(.*?)^endmodule\b', re.M | re.S)
INSTANCE = re.compile(r'^\s*(GTP_\w+|ag_hmic_\w+)\s+(?:/\*.*?\*/\s*)?(?:#\s*\(.*?\)\s*)?(\\\S+|[A-Za-z_]\w*)\s*\((.*?)\);', re.M | re.S)
ANNOT = re.compile(r'SDC constraint\s*:\s*\(object ([^)]+)\)\s*\(id (\d+)\)\s*\((from|to)\)\s*\((\d+)\s*:\s*([^\n]+)\)')


def normalized_path(value):
    value = str(value).replace('\\', '/').strip().lower()
    return re.sub(r'^/mnt/([a-z])/', lambda m: m.group(1) + ':/', value)


def mapped_cells(text):
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
    def visit(module, prefix):
        for row in modules[module]:
            path = prefix + '/' + row['name'] if prefix else row['name']
            if row['type'] in modules:
                visit(row['type'], path)
            elif row['type'].startswith('GTP_'):
                cells.append(dict(row, path=path))
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
        return [c for c in cells if fnmatch.fnmatchcase(re.sub(r'\[[0-2]\]$', '', c['path']), pattern)]
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
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--require-precise', action='store_true')
    parser.add_argument('--require-bounded-gray', action='store_true')
    args = parser.parse_args()
    text = args.netlist.read_text(errors='replace')
    fdc = args.fdc.read_text(errors='replace')
    cells = mapped_cells(text)
    report = {'passed': False, 'netlist': str(args.netlist.resolve()), 'netlist_sha256': sha(args.netlist),
              'fdc': str(args.fdc.resolve()), 'fdc_sha256': sha(args.fdc),
              'scope': 'actual PDS mapped-cell and emitted SDC binding audit, not timing closure', 'groups': []}
    try:
        gray = payload = 0
        for lineno, line in enumerate(fdc.splitlines(), 1):
            if not line.startswith('set_max_delay ') or 'bridge/' not in line:
                continue
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
                if len(dst) != 3 or any(not c['type'].startswith('GTP_DFF') for c in dst):
                    raise ValueError('Gray first synchronizer must be exactly three real FFs')
                dst_matches = [re.fullmatch(r'(.*/)(wgray_r1|rgray_w1)\[([0-2])\]', c['path']) for c in dst]
                if not all(dst_matches):
                    raise ValueError('unexpected Gray destination names')
                roots = {m.group(1) for m in dst_matches}
                dest_buses = {m.group(2) for m in dst_matches}
                if len(roots) != 1 or len(dest_buses) != 1 or {m.group(3) for m in dst_matches} != {'0', '1', '2'}:
                    raise ValueError('first-sync triplet is incomplete or spans FIFOs')
                root = next(iter(roots))
                dest_bus = next(iter(dest_buses))
                source_bus = 'wgray' if dest_bus == 'wgray_r1' else 'rgray'
                sync_suffix = '_r' if source_bus == 'wgray' else '_w'
                true_source = [c for c in src if re.fullmatch(re.escape(root + source_bus) + r'\[[0-2]\]', c['path'])]
                extras = [c for c in src if c not in true_source]
                true_bits = {re.search(r'\[([0-2])\]$', c['path']).group(1) for c in true_source}
                bounded = len(true_source) == 3 and true_bits == {'0', '1', '2'}
                bounded = bounded and all(c['type'].startswith('GTP_DFF') for c in src)
                bounded = bounded and all(re.fullmatch(re.escape(root + source_bus + sync_suffix) + r'[12]\[[0-2]\]', c['path']) for c in extras)
                # Extra endpoints must use exactly the first-sync clock. They
                # add a conservative bound, never a setup false-path exception.
                first_clocks = {re.search(r'\.CLK\s*\(([^)]+)\)', c['pins']).group(1).strip() for c in dst}
                extra_clocks = {re.search(r'\.CLK\s*\(([^)]+)\)', c['pins']).group(1).strip() for c in extras}
                bounded = bounded and len(first_clocks) == 1 and extra_clocks.issubset(first_clocks)
                precise = bounded and not extras
                if args.require_precise and not precise:
                    raise ValueError('Gray source contains synchronizer stages/extra cells')
                if args.require_bounded_gray and not bounded:
                    raise ValueError('true Gray triplet missing or unexpected extra endpoints/clocks')
                if float(limit) > 7:
                    raise ValueError('Gray delay exceeds7ns')
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
                           clock_frequency_mhz=float(actual_clock.group(1)), clock_report=str(timing.resolve()), clock_report_sha256=sha(timing),
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
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('QUEUE4_CDC_REAL_BINDINGS_PASS', args.output)


if __name__ == '__main__':
    main()
