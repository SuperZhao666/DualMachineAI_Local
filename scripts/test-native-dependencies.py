"""Current build-graph dependency verification; no compiler or real tree cleanup."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('dependencies', Path(__file__).with_name('repair-native-dependencies.py'))
dependencies = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dependencies)


class DependencyInventoryTests(unittest.TestCase):
    def state(self, count=1, status='VALID', present=True, graph=True):
        with tempfile.TemporaryDirectory() as value:
            build = Path(value)
            names = ['CMakeFiles/current.dir/new.cpp.obj', 'CMakeFiles/current.dir/retired/path/old.cpp.obj']
            for name in names:
                path=build/name; path.parent.mkdir(parents=True,exist_ok=True); path.write_bytes(b'object')
            key=dependencies.object_key(names[0])
            output=f'{names[0]}: #deps {count}, deps mtime 1 ({status})\n    project/header.hpp\n\n' if present else ''
            mapping={key:{'source':Path(__file__)}} if graph else {}
            with patch.object(dependencies,'run_ninja',return_value=output), \
                 patch.object(dependencies,'compilation_sources',return_value=mapping), \
                 patch.object(dependencies,'classify_zero_dependency',return_value={'allowed':False,'reason':'test_project_header'}):
                return dependencies.dependency_state(build,Path('ninja'),False)

    def test_retired_object_does_not_block_verified_current_graph(self):
        state=self.state()
        self.assertEqual(state['compiled_objects'],1)
        self.assertEqual(len(state['ignored_obsolete_objects']),1)
        self.assertEqual(state['issues'],[])

    def test_active_object_without_record_is_rejected(self):
        self.assertEqual(self.state(present=False)['issues'][0]['reason'],'missing_dependency_record')

    def test_active_project_object_zero_dependencies_is_rejected(self):
        self.assertEqual(self.state(count=0)['issues'][0]['reason'],'zero_header_dependencies')

    def test_active_stale_record_is_rejected(self):
        self.assertEqual(self.state(status='STALE')['issues'][0]['reason'],'stale_dependency_record')

    def test_missing_graph_fails_closed(self):
        self.assertRaises(dependencies.DependencyError,self.state,graph=False)


if __name__=='__main__': unittest.main(verbosity=2)
