"""C manifest/nonce/profile/batch validation without Bluetooth or a lens."""
from pathlib import Path
import hashlib
import json
import struct
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
from types import SimpleNamespace
from contextlib import redirect_stdout
import io

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
import decoder_speed_test as tool
import test_tool


class CapsuleToolTests(unittest.TestCase):
    def fixture(self, folder, kind='mixed'):
        raw=bytes(range(128))
        manifest=dict(abi=1,name=kind,kind=kind,decoder='59c66b1',bytes=len(raw),
            sha256=hashlib.sha256(raw).hexdigest(),text_bounds=[0,64],
            exports=dict(zip(tool.CAPSULE_EXPORTS,range(0,12,2))),
            profile_functions={'function':0} if kind.startswith('profile') else {})
        folder.mkdir(parents=True,exist_ok=True)
        path=folder/'capsule.json';path.write_text(json.dumps(manifest),encoding='utf-8')
        (folder/'capsule.bin').write_bytes(raw)
        (folder/'checks.json').write_text(json.dumps(dict(result='PASS',sha256=manifest['sha256'],lld=True,pc_y=True)),encoding='utf-8')
        return path,raw,manifest

    def test_c_nonce_message_and_begin_layout(self):
        payload=tool.message(tool.BEGIN,321,b'data',0x12345678)
        self.assertEqual(payload,b'\x1fDS\x03\x01A\x01xV4\x12data')
        for nonce in (0,-1,0x100000000):
            with self.assertRaises(ValueError):tool.message(tool.BEGIN,1,b'',nonce)
        with tempfile.TemporaryDirectory() as temporary:
            path,raw,manifest=self.fixture(Path(temporary))
            capsule=tool.load_capsule(path)
            begin=tool.capsule_begin(tool.PROFILES[0],capsule)
            self.assertEqual(len(begin),88)
            self.assertEqual(struct.unpack_from('<IIHH',begin),(tool.CLIP_BYTES,tool.CLIP_CRC,320,192))
            self.assertEqual(struct.unpack_from('<5I',begin,12),(128,0,64,65536,0))
            self.assertEqual(begin[56:],bytes.fromhex(manifest['sha256']))

    def test_invalid_proof_hash_bounds_exports_and_profile_fail_before_connect(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)
            path,raw,manifest=self.fixture(folder)
            for field,value in [('abi',2),('bytes',129),('sha256','0'*64),('decoder','other'),
                                ('text_bounds',[0,129]),('kind','unknown'),('profile_functions',{'bad':1}),
                                ('name','../../outside')]:
                bad=dict(manifest);bad[field]=value;path.write_text(json.dumps(bad),encoding='utf-8')
                with self.assertRaises(ValueError):tool.load_capsule(path)
            for offset in (1,64,0):
                bad=dict(manifest,exports=dict(manifest['exports']));bad['exports']['ds_init']=offset
                path.write_text(json.dumps(bad),encoding='utf-8')
                with self.assertRaises(ValueError):tool.load_capsule(path)
            path.write_text(json.dumps(manifest),encoding='utf-8')
            (folder/'checks.json').write_text('{}',encoding='utf-8')
            with self.assertRaises(ValueError):tool.load_capsule(path)
            (folder/'capsule.bin').write_bytes(raw[:-1])
            with self.assertRaises(ValueError):tool.load_capsule(path)

    def test_c_profile_short_run_checks_all_hashes_and_excludes_only_warmup(self):
        words,ref=test_tool.ToolTests().valid_run()
        words=words[:tool.HEADER_WORDS+64*tool.FRAME_WORDS]
        words[0]=tool.C_MAGIC;words[1]=4|256;words[3]=len(words);words[5]=1;words[37]=1;words[114]=1
        summary=tool.summarize(words,ref,'capsule-ram',1,0)
        self.assertEqual(len(summary['samples']),64)
        self.assertEqual(summary['measured_frames'],32)
        self.assertEqual(summary['included_frames'],32)
        words[129]^=1
        with self.assertRaises(ValueError):tool.summarize(words,ref,'capsule-ram',1,0)
        for skip,uncached in ((0,0),(1,1)):
            with self.assertRaises(ValueError):tool.summarize(words,ref,'capsule-ram',skip,uncached)

    def test_new_nonce_clears_old_results_without_new_connection(self):
        client=tool.Client(None,'left');client.nonce=1;client.results={1:2};seq=client.next_sequence
        client.new_session()
        self.assertNotEqual(client.nonce,1);self.assertEqual(client.results,{})
        self.assertEqual(client.next_sequence,seq)

    def test_c_upload_fragments_nonce_and_profile_reads_round_trip(self):
        transport=test_tool.FakeTransport()
        client=tool.Client(transport,'left'); client.max_write=20; client.nonce=0x12345678
        with redirect_stdout(io.StringIO()):
            self.assertEqual(client.command(tool.HELLO,result_index=65535),tool.C_MAGIC)
            client.command(tool.CAP_WRITE,struct.pack('<I',2048)+bytes(range(256))*8)
            self.assertEqual(client.command(tool.PROFILE_READ,struct.pack('<H',17),result_index=17),1234)
        payload=transport.received[-2]
        self.assertEqual(len(payload),2063)
        self.assertEqual(payload[7:11],struct.pack('<I',client.nonce))
        self.assertEqual(payload[15:],bytes(range(256))*8)
        self.assertTrue(all(len(packet)<=20 for packet in transport.packets))

    def test_color_capsule_batch_is_rejected_before_reading_or_connecting(self):
        args=SimpleNamespace(output_dir='unused',skip_chroma=[0,1])
        with patch.object(tool,'measure') as connect:
            with self.assertRaisesRegex(ValueError,'skip_chroma=1'):tool.capsule_batch(args)
        connect.assert_not_called()

    def test_c_bridge_connection_is_rejected_before_opening_it(self):
        _,reference=test_tool.ToolTests().valid_run()
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary); clip=folder/'clip'; ref=folder/'ref'
            clip.write_bytes(b'fake'); ref.write_text(json.dumps(reference),encoding='utf-8')
            case=SimpleNamespace(capsule='unused',setup='capsule-ram',skip_chroma=1,uncached=0,
                                 clip=clip,reference=ref)
            args=SimpleNamespace(connection='unused')
            with patch.object(tool,'load_capsule'),patch.object(tool,'check_clip',return_value=tool.PROFILES[0]), \
                    patch.object(tool,'parse_connection_string',return_value={'method':'droidbridge'}), \
                    patch.object(tool,'Bridge') as bridge,redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(ValueError,'direct local'):tool.measure(args,[case])
            bridge.assert_not_called()

    def test_profile_paged_counts_and_invalid_stack_are_explicit(self):
        client=Mock();client.command.side_effect=[3,5,1,7,2]
        words=[0]*128;words[110]=1;words[111]=4;words[112]=7;words[113]=13328
        manifest={'profile_functions':{'name':0}}
        result=tool.read_profile(client,words,manifest)
        self.assertFalse(result['valid']);self.assertEqual(result['flags'],4)
        self.assertEqual(result['rows'][0]['inclusive_cycles'],(1<<32)+5)
        self.assertEqual(result['rows'][0]['exclusive_cycles'],(2<<32)+7)
        self.assertTrue(result['ranking_only']);self.assertEqual(client.command.call_count,5)

    def test_capsule_matrix_closes_each_case_one_connection_and_preserves_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)
            paths=[self.fixture(folder/name,name)[0] for name in ('mixed','full')]
            clip=folder/'clip';clip.write_bytes(b'fake')
            args=SimpleNamespace(output_dir=folder/'results',capsule=paths,clip=[clip],reference=[folder/'ref'],lens='left')
            with patch.object(tool,'check_clip',return_value=tool.PROFILES[0]),patch.object(tool,'measure',return_value=[{}]) as measure,redirect_stdout(io.StringIO()):
                tool.capsule_batch(args)
            cases=measure.call_args.args[1]
            self.assertEqual(len(cases),2);self.assertTrue(all(c.skip_chroma==1 and c.setup=='capsule-ram' for c in cases))
            self.assertEqual([c.capsule for c in cases],paths)
            with self.assertRaises(FileExistsError):tool.capsule_batch(args)


if __name__=='__main__':unittest.main()
