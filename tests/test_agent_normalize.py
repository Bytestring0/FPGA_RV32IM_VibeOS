import importlib.util
import json
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('normalize',Path(__file__).resolve().parents[1]/'scripts/agent_normalize.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class AdapterTest(unittest.TestCase):
    def call(self,arguments,name='calculate',ident='c1'):
        return {'id':ident,'type':'function','function':{'name':name,'arguments':arguments}}
    def test_exact_arguments(self):
        ident,envelope=module.normalize(self.call('{"expression":"12+23"}'))
        self.assertEqual(ident,'c1');self.assertEqual(json.loads(envelope),{'name':'calculate','arguments':{'expression':'12+23'}})
    def test_unicode(self):
        _,text=module.normalize(self.call('{"value":10,"unit":"分钟","label":"学习"}','start_timer'))
        self.assertEqual(json.loads(text)['arguments']['unit'],'分钟')
    def test_reject(self):
        for args in ('{"expression":"1","expression":"2"}','{"x":NaN}','{"x":null}','{"x":true}','{"x":{}}'):
            with self.subTest(args=args),self.assertRaises(ValueError):module.normalize(self.call(args))
    def test_id_not_truncated(self):
        with self.assertRaises(ValueError):module.normalize(self.call('{}',ident='a'*33))
    def test_no_nul_or_surrogate(self):
        for text in ('{"text":"\\u0000"}','{"text":"\\ud800"}'):
            with self.assertRaises((ValueError,UnicodeError)):module.normalize(self.call(text,'append_note'))

if __name__=='__main__':unittest.main()
