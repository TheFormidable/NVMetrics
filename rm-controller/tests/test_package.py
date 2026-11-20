import json
import os
import plistlib
import sys
import tempfile
import unittest
import shutil
from contextlib import ExitStack
from pathlib import Path
from unittest.mock import patch,Mock

import package_install as pkg
import physical
import runtime
from protocol import decode_request,ProtocolError


class PackagingTests(unittest.TestCase):
    def test_daemon_uses_private_runtime_thermal_supervisor(self):
        d=plistlib.loads(pkg.launch_plist(501))
        self.assertEqual(d['ProgramArguments'][0],str(pkg.ROOT/'Runtime/nvmetrics-runtime'))
        self.assertEqual(d['ProgramArguments'][1],'server')
        self.assertNotIn('--load-test',d['ProgramArguments'])
        self.assertIn('--thermal-supervised',d['ProgramArguments'])
        self.assertIn('RM_V08_EXPERIMENT',d['ProgramArguments'])
        self.assertFalse(d['RunAtLoad'])
        self.assertEqual(d['Sockets']['Listener']['SockPathName'],str(pkg.ROOT/'control.sock'))
        self.assertIn('--continuous',d['ProgramArguments'])

    def test_unqualified_gpu_read_only_daemon(self):
        d=plistlib.loads(pkg.launch_plist(501,False))
        for flag in ('--continuous','--enable-physical','--thermal-supervised'):
            self.assertNotIn(flag,d['ProgramArguments'])

    def test_frozen_rescue_is_independent_process(self):
        p=physical.Physical(Path('/fake/agent'))
        with patch.object(sys,'frozen',True,create=True):
            self.assertEqual(p.rescue_command(),[str(pkg.ROOT/'Runtime/nvmetrics-runtime'),'rescue'])
        self.assertEqual(p.rescue_command()[0],'/usr/bin/python3')

    def test_unprivileged_runtime_denies_privileged_roles(self):
        for role in ('server','rescue','install','unknown'):
            with patch.object(sys,'argv',['runtime',role]),patch.object(os,'geteuid',return_value=501):
                self.assertEqual(runtime.main(),2)

    def test_runtime_routes_client_without_root(self):
        with patch.object(sys,'argv',['runtime','client','ping']),patch.object(os,'geteuid',return_value=501),patch('nvmetrics_ipc_v08.main',return_value=0) as mock:
            self.assertEqual(runtime.main(),0);mock.assert_called_once_with()

    def test_floor_still_enforced(self):
        for pct in (0,5,10,25,29):
            raw=(json.dumps({'v':1,'op':'manual_lower','pct':pct,'session':'a'*32})+'\n').encode()
            with self.assertRaises(ProtocolError): decode_request(raw)

    def test_tree_digest_changes_and_refuses_escaping_links(self):
        with tempfile.TemporaryDirectory() as d:
            t=Path(d);(t/'data').write_text('one');(t/'link').symlink_to('data')
            first=pkg.tree_digest(t)
            (t/'data').write_text('two')
            self.assertNotEqual(first,pkg.tree_digest(t))
            (t/'external').symlink_to('/etc/passwd')
            with self.assertRaises(RuntimeError):pkg.tree_digest(t)

    def test_inspect_auto_does_not_write(self):
        records=[{'policy_word_candidate':0,'requested_q16_candidate':0,'rpm_min_candidate':1150,'rpm_max_candidate':3700}]*2
        snapshot={'operation':'rm-snapshot','get_regular_status':0,'get_default_status':0,
                  **{n:{'layout_matches_two':True,'records':records} for n in ('regular','default')}}
        with patch.object(pkg,'run') as run:
            run.return_value.returncode=0;run.return_value.stdout=json.dumps(snapshot)
            pkg.inspect_auto(Path('/agent'),qualified=True)
            run.assert_called_once_with([Path('/agent'),'fan','rm-inspect'],10)
            records[0]['policy_word_candidate']=1
            run.return_value.stdout=json.dumps(snapshot)
            with self.assertRaises(RuntimeError):pkg.inspect_auto(Path('/agent'),qualified=True)

    def test_unknown_gpu_rejected(self):
        with patch.object(pkg,'run') as run:
            run.return_value.returncode=0
            for gpus in ([{'sppci_model':'GeForce RTX 5080'}],[{'sppci_model':'NVIDIA GeForce RTX 5050'},{'sppci_model':'Other'}],[]):
                run.return_value.stdout=json.dumps({'SPDisplaysDataType':gpus})
                with self.assertRaises(RuntimeError):pkg.qualified_gpu()
            run.return_value.stdout=json.dumps({'SPDisplaysDataType':[{'sppci_model':'NVIDIA GeForce RTX 5050'}]})
            pkg.qualified_gpu()


class TransactionTests(unittest.TestCase):
    """Simulated macOS installation with real temp-file mutations, no root/GPU."""
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.d=Path(self.tmp.name)
        self.root=self.d/'Library'/'PrivilegedHelperTools'/'org.nvmetrics.rmguard';self.root.mkdir(parents=True)
        self.app=self.d/'Applications'/'NVMetrics.app';self.app.parent.mkdir()
        self.plist=self.d/'Library'/'LaunchDaemons'/'org.nvmetrics.rmguard.plist';self.plist.parent.mkdir()
        self.man=self.root/'.v012-manifest.json';self.receipt=self.root/'.v014-package.json'
        self.back=self.root/pkg.BACK.name;self.rt=self.root/'Runtime'
        self.payload=self.d/'payload';app=self.payload/'NVMetrics.app';source=self.payload/'service';source.mkdir(parents=True)
        res=app/'Contents/Resources';rt=res/'GuardTools/Runtime';rt.mkdir(parents=True)
        (app/'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier':'org.nvmetrics.app'}))
        (res/'nvmetrics-agent').write_bytes(pkg.MARKER);(rt/'nvmetrics-runtime').write_text('test runtime');(rt/'nvmetrics-runtime').chmod(0o755)
        for n in pkg.FILES:
            if n!='nvmetrics-agent':(source/n).write_text('payload '+n)
        (self.payload/'integrity.json').write_text(json.dumps({'app':pkg.tree_digest(app),'service':pkg.tree_digest(source)}))
        st=ExitStack();self.addCleanup(st.close)
        st.enter_context(patch.multiple(pkg,ROOT=self.root,APP=self.app,RUNTIME=self.rt,RECEIPT=self.receipt,BACK=self.back))
        st.enter_context(patch.multiple(pkg.base,ROOT=self.root,PLIST=self.plist,MAN=self.man,LOCKS=(self.root/'control.lock',self.d/'lease.lock',self.d/'guard.lock')))
        st.enter_context(patch.object(pkg.base,'safe_dir'));st.enter_context(patch.object(pkg.base,'safe_file'))
        st.enter_context(patch.object(pkg,'secure_tree'));st.enter_context(patch.object(pkg,'qualified_gpu'))
        self.inspect=st.enter_context(patch.object(pkg,'inspect_auto'))
        self.loaded=st.enter_context(patch.object(pkg,'loaded',return_value=False))
        self.start=st.enter_context(patch.object(pkg,'start_job'));self.stop=st.enter_context(patch.object(pkg,'stop_job'))
        self.client=st.enter_context(patch.object(pkg,'verify_client'))
        self.acquire=st.enter_context(patch.object(pkg,'acquire',side_effect=lambda p:os.open(p,os.O_CREAT|os.O_RDWR,0o600)))
        def atomic(p,data,mode): p.write_bytes(data);p.chmod(mode)
        st.enter_context(patch.object(pkg.base,'atomic',side_effect=atomic))
        def run(args,timeout=20):
            if args[0]=='/usr/bin/ditto':shutil.copytree(args[1],args[2],symlinks=True)
            return Mock(returncode=0)
        st.enter_context(patch.object(pkg,'run',side_effect=run))

    def test_fresh_and_idempotent(self):
        pkg.install(self.payload,501)
        self.assertTrue(self.app.exists());self.assertTrue(self.rt.exists());self.assertTrue(self.receipt.exists())
        self.assertTrue(self.plist.exists());self.assertEqual(pkg.installed_state(501),'same')
        pkg.install(self.payload,501)
        self.assertEqual(self.start.call_count,2)
        self.assertEqual(self.client.call_count,2)
        self.assertEqual(len(self.stop.call_args_list),0)

    def test_fresh_launch_failure_rolls_back(self):
        self.start.side_effect=RuntimeError('bootstrap failed')
        with self.assertRaisesRegex(RuntimeError,'bootstrap failed'):pkg.install(self.payload,501)
        for p in (self.app,self.rt,self.receipt,self.plist,self.man,self.back):self.assertFalse(p.exists(),p)
        for n in pkg.FILES:self.assertFalse((self.root/n).exists())

    def test_fresh_client_handshake_failure_rolls_back(self):
        self.client.side_effect=RuntimeError('IPC failed')
        with self.assertRaisesRegex(RuntimeError,'IPC failed'):pkg.install(self.payload,501)
        for p in (self.app,self.rt,self.receipt,self.plist,self.man,self.back):self.assertFalse(p.exists(),p)

    def test_migration_bootstrap_failure_restores_prior_app_and_root(self):
        shutil.copytree(self.payload/'NVMetrics.app',self.app)
        (self.app/'old').write_text('original')
        for n in pkg.FILES:(self.root/n).write_bytes(b'old '+n.encode())
        self.plist.write_bytes(b'old plist');self.man.write_bytes(b'old manifest')
        self.loaded.return_value=True
        self.start.side_effect=[RuntimeError('bootstrap failed'),None]
        with patch.object(pkg,'installed_state',return_value='legacy'):
            with self.assertRaisesRegex(RuntimeError,'bootstrap failed'):pkg.install(self.payload,501)
        self.assertTrue((self.app/'old').exists());self.assertFalse(self.rt.exists());self.assertFalse(self.receipt.exists())
        self.assertEqual(self.plist.read_bytes(),b'old plist');self.assertEqual(self.man.read_bytes(),b'old manifest')
        for n in pkg.FILES:self.assertEqual((self.root/n).read_bytes(),b'old '+n.encode())
        self.assertEqual(self.start.call_count,2)

    def test_active_physical_lease_blocks_before_bootout(self):
        def lock(path):
            if path==pkg.base.LOCKS[1]:raise BlockingIOError('busy')
            return os.open(path,os.O_CREAT|os.O_RDWR,0o600)
        self.acquire.side_effect=lock
        with self.assertRaises(BlockingIOError):pkg.install(self.payload,501)
        self.stop.assert_not_called();self.start.assert_not_called()
        self.assertFalse(self.app.exists());self.assertFalse(self.plist.exists())

    def simulate_v014(self,version='0.14.0'):
        pkg.install(self.payload,501)
        self.back.rename(self.root/'original-v014-backup')
        marker={'0.14.0':pkg.LEGACY_MARKER,'0.14.3':pkg.ADAPTIVE_MARKER,'0.14.4':pkg.ADAPTIVE_MARKER,'0.14.5':pkg.ADAPTIVE_MARKER,'0.14.6':pkg.FRESHNESS_MARKER,'0.14.7':pkg.SESSION_MARKER,'0.14.8':pkg.V0148_MARKER}.get(version,pkg.PREVIOUS_MARKER)
        (self.root/'nvmetrics-agent').write_bytes(marker)
        (self.app/'Contents/Resources/nvmetrics-agent').write_bytes(marker)
        (self.rt/'nvmetrics-runtime').write_text('previous frozen runtime')
        record=json.loads(self.receipt.read_text());record['version']=version
        record['app']=pkg.tree_digest(self.app);record['runtime']=pkg.tree_digest(self.rt)
        record['files']['nvmetrics-agent']=pkg.sha(self.root/'nvmetrics-agent')
        self.receipt.write_text(json.dumps(record))
        self.loaded.return_value=True
        return record

    def test_upgrade_from_verified_v014(self):
        self.simulate_v014()
        self.assertEqual(pkg.installed_state(501),'upgrade')
        pkg.install(self.payload,501)
        self.assertEqual(pkg.installed_state(501),'same')
        self.assertEqual((self.back/'Runtime/nvmetrics-runtime').read_text(),'previous frozen runtime')
        self.assertTrue((self.root/'original-v014-backup').exists())
        self.assertTrue((self.app.parent/'.NVMetrics.app.before-v0160').exists())

    def test_upgrade_from_verified_v0141(self):
        self.simulate_v014('0.14.1')
        self.assertEqual(pkg.installed_state(501),'upgrade')
        pkg.install(self.payload,501)
        self.assertEqual(pkg.installed_state(501),'same')
        self.assertEqual((self.back/'Runtime/nvmetrics-runtime').read_text(),'previous frozen runtime')

    def test_upgrade_from_verified_v0142(self):
        self.simulate_v014('0.14.2')
        self.assertEqual(pkg.installed_state(501),'upgrade')
        pkg.install(self.payload,501)
        self.assertEqual(pkg.installed_state(501),'same')

    def test_upgrade_from_verified_v0143(self):
        self.simulate_v014('0.14.3')
        self.assertEqual(pkg.installed_state(501),'upgrade')
        pkg.install(self.payload,501)
        self.assertEqual(pkg.installed_state(501),'same')

    def test_upgrade_from_verified_v0144(self):
        self.simulate_v014('0.14.4')
        self.assertEqual(pkg.installed_state(501),'upgrade')
        pkg.install(self.payload,501)
        self.assertEqual(pkg.installed_state(501),'same')

    def test_upgrade_from_verified_v0146(self):
        self.simulate_v014('0.14.6')
        self.assertEqual(pkg.installed_state(501),'upgrade')
        pkg.install(self.payload,501)
        self.assertEqual(pkg.installed_state(501),'same')

    def test_upgrade_from_verified_v0145(self):
        self.simulate_v014('0.14.5')
        self.assertEqual(pkg.installed_state(501),'upgrade')
        pkg.install(self.payload,501)
        self.assertEqual(pkg.installed_state(501),'same')

    def test_upgrade_from_verified_v0147(self):
        # What a real 0.14.7 leaves behind after it installed: its own backups, under its own names.
        self.simulate_v014('0.14.7')
        old_back=self.root/'.v0147-package-backup';old_back.mkdir()
        old_app=self.app.parent/'.NVMetrics.app.before-v0147';old_app.mkdir()
        self.assertEqual(pkg.installed_state(501),'upgrade')
        pkg.install(self.payload,501)
        self.assertEqual(pkg.installed_state(501),'same')
        self.assertEqual(json.loads(self.receipt.read_text())['version'],'0.16.0')
        self.assertEqual((self.back/'Runtime/nvmetrics-runtime').read_text(),'previous frozen runtime')
        self.assertIn(pkg.MARKER,(self.root/'nvmetrics-agent').read_bytes())
        self.assertTrue(old_back.exists());self.assertTrue(old_app.exists())

    def test_v0147_receipt_with_another_agent_is_refused(self):
        self.simulate_v014('0.14.7')
        (self.root/'nvmetrics-agent').write_bytes(pkg.FRESHNESS_MARKER)
        record=json.loads(self.receipt.read_text())
        record['files']['nvmetrics-agent']=pkg.sha(self.root/'nvmetrics-agent')
        self.receipt.write_text(json.dumps(record))
        with self.assertRaisesRegex(RuntimeError,'wrong previous agent'):pkg.installed_state(501)

    def test_upgrade_v0148_without_privileged_setting(self):
        record=self.simulate_v014('0.14.8')
        (self.root/'nvmetrics-setting.sh').unlink()
        del record['files']['nvmetrics-setting.sh']
        self.receipt.write_text(json.dumps(record))
        self.assertEqual(pkg.installed_state(501),'upgrade')
        pkg.install(self.payload,501)
        self.assertEqual(pkg.installed_state(501),'same')
        self.assertTrue((self.root/'nvmetrics-setting.sh').exists())

    def test_upgrade_from_verified_v0150(self):
        record=self.simulate_v014('0.15.0')
        (self.root/'nvmetrics-agent').write_bytes(pkg.V0150_MARKER)
        (self.app/'Contents/Resources/nvmetrics-agent').write_bytes(pkg.V0150_MARKER)
        record['files']['nvmetrics-agent']=pkg.sha(self.root/'nvmetrics-agent')
        record['app']=pkg.tree_digest(self.app)
        self.receipt.write_text(json.dumps(record))
        (self.root/'.v0150-package-backup').mkdir()
        (self.app.parent/'.NVMetrics.app.before-v0150').mkdir()
        self.assertEqual(pkg.installed_state(501),'upgrade')
        pkg.install(self.payload,501)
        self.assertEqual(pkg.installed_state(501),'same')
        self.assertTrue((self.root/'.v0150-package-backup').exists())

    def test_one_revision_everywhere(self):
        # The installer accepts the service only when its ping names this revision, and the package only when its
        # agent carries this revision's marker: a bump that misses one of these fails every install.
        here=Path(pkg.__file__).resolve().parent
        version=pkg.VERSION;marker=pkg.MARKER.decode()
        self.assertIn("'revision':'%s'"%version,(here/'service'/'server.py').read_text())
        build=(here/'build_package.sh').read_text()
        self.assertIn("grep '%s'"%marker,build)
        self.assertIn("obj['CFBundleShortVersionString']='%s';obj['CFBundleVersion']='%s'"%(version,version),build)
        self.assertIn('--version %s '%version,build)
        self.assertIn('NVMetrics-%s-Candidate-Intel.pkg'%version,build)
        self.assertIn(marker,(here/'agent'/'nvmetrics_agent.c').read_text())
        for name in ('Verify.command','verify_package.sh'):
            self.assertIn('NVMetrics-%s-Candidate-Intel.pkg'%version,(here/name).read_text())
        # the folder the build writes to by default is the one the verifier looks in by default
        digits=version.replace('.','')
        for name in ('Build.command','build_package.sh','Verify.command','verify_package.sh'):
            self.assertIn('NVMetrics-v%s-release'%digits,(here/name).read_text())
        # each revision keeps its own backups: under a name another revision already used, the upgrade is refused
        installer=Path(pkg.__file__).read_text()
        self.assertIn("BACK = ROOT/'.v%s-package-backup'"%digits,installer)
        self.assertIn("'.NVMetrics.app.before-v%s'"%digits,installer)

    def test_upgrade_handshake_failure_restores_runtime_app_and_receipt(self):
        record=self.simulate_v014()
        self.client.side_effect=RuntimeError('upgrade handshake failed')
        with self.assertRaisesRegex(RuntimeError,'upgrade handshake failed'):pkg.install(self.payload,501)
        self.assertEqual(json.loads(self.receipt.read_text()),record)
        self.assertEqual(pkg.tree_digest(self.rt),record['runtime'])
        self.assertEqual(pkg.tree_digest(self.app),record['app'])
        self.assertEqual(pkg.installed_state(501),'upgrade')


if __name__=='__main__':unittest.main()
