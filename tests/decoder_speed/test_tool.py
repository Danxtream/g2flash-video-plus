"""PC protocol framing and correctness checks; no Bluetooth connection."""
from pathlib import Path
import struct
import sys
import unittest
import queue
from contextlib import redirect_stdout, redirect_stderr
import io
import asyncio
import re
import tempfile
from types import SimpleNamespace
import zlib
from unittest.mock import AsyncMock, Mock, patch

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
import decoder_speed_test as tool


def result_frame(body):
    return bytes((0xaa,0x12,7,len(body)+2,1,1,0xf0,0))+body+tool.crc16(body)


class ToolTests(unittest.TestCase):
    def test_result_minimum_mtu_and_crc(self):
        frame=result_frame(struct.pack('<BBHHI',5,1,321,17,0x12345678))
        self.assertEqual(len(frame),20)
        self.assertEqual(tool.parse_result(frame),(1,321,17,0x12345678))
        self.assertIsNone(tool.parse_result(frame[:-1]+bytes([frame[-1]^1])))

    def test_wrong_channel_kind_length_and_lens_rejected(self):
        body=struct.pack('<BBHHI',5,1,321,17,9)
        self.assertIsNone(tool.parse_result(result_frame(body+b'\0')))
        self.assertIsNone(tool.parse_result(result_frame(bytes([1])+body[1:])))
        self.assertIsNone(tool.parse_result(result_frame(bytes([5,3])+body[2:])))

    def test_command_version_and_session(self):
        self.assertEqual(tool.message(tool.READ,321,struct.pack('<H',17)),b'\x1fDS\x02\x05A\x01\x11\x00')
        with self.assertRaises(ValueError): tool.message(tool.RUN,65536)

    def valid_run(self):
        words=[0]*tool.RESULT_WORDS
        words[:7]=[tool.MAGIC,0,0,len(words),32,5,tool.CLIP_CRC]
        words[37]=2
        words[15]=tool.FRAME_WORDS
        words[12:15]=[320,192,tool.CLIP_BYTES]
        hashes=list(range(32))
        reference=dict(clip_sha256=tool.CLIP_SHA256,clip_crc32=tool.CLIP_CRC,decoder='59c66b1',
                       hashes={'0':hashes,'1':hashes},dimensions=[320,192],frames=32)
        for repeat in range(7):
            for frame in range(32):
                i=128+(repeat*32+frame)*tool.FRAME_WORDS
                ms=10 if repeat<2 else 2
                words[i:i+tool.FRAME_WORDS]=[250000*ms,frame,ms,250000,250000]
        return words,reference

    def test_warmup_hashes_checked_but_excluded_from_timing(self):
        words,ref=self.valid_run()
        summary=tool.summarize(words,ref,'full-mram',0,0)
        self.assertEqual(summary['median_ms'],2)
        self.assertEqual(summary['median_tick_ms'],2)
        self.assertEqual(len(summary['samples']),224)
        self.assertEqual(summary['included_frames'],160)

    def test_any_warmup_or_measured_mismatch_voids_entire_run(self):
        for repeat in (0,1,2,6):
            words,ref=self.valid_run(); words[128+repeat*32*tool.FRAME_WORDS+1]^=1
            with self.assertRaises(ValueError): tool.summarize(words,ref,'full-mram',0,0)

    def test_bad_status_dimensions_setup_and_bounds_rejected(self):
        for index,value in ((0,0),(1,1),(2,13),(3,0),(4,31),(5,4),(6,0),(15,4),(37,0),(12,640)):
            words,ref=self.valid_run(); words[index]=value
            with self.assertRaises(ValueError): tool.summarize(words,ref,'full-mram',0,0)

    def test_clock_drift_and_tick_disagreement_are_flagged_not_fatal(self):
        for field,value,reason in ((4,261000,'clock changed'),(0,1500000,'cycles and ticks'),
                                   (3,0,'calibration'),(0,0,'out of range'),(2,2001,'out of range')):
            words,ref=self.valid_run()
            at=tool.HEADER_WORDS+tool.WARMUPS*tool.FRAMES*tool.FRAME_WORDS
            words[at+field]=value
            summary=tool.summarize(words,ref,'full-mram',0,0)
            self.assertTrue(summary['valid'])
            self.assertEqual(summary['excluded_frames'],1)
            self.assertEqual(summary['included_frames'],159)
            row=next(r for r in summary['samples'] if r['repeat']==2 and r['frame']==0)
            self.assertTrue(row['flagged'])
            self.assertTrue(any(reason in flag for flag in row['timing_flags']))
            self.assertEqual(row['hash'],0)
            self.assertEqual(row['clock_before'],words[at+3])
            self.assertEqual(row['clock_after'],words[at+4])

    def test_four_percent_boundary_and_warmup_flags(self):
        words,ref=self.valid_run()
        words[132]=260000  # Exactly 4%, accepted; a warmup, outside summaries.
        self.assertEqual(tool.summarize(words,ref,'full-mram',0,0)['flagged_frames'],0)
        words[132]=260001
        summary=tool.summarize(words,ref,'full-mram',0,0)
        self.assertEqual(summary['warmup_flagged_frames'],1)
        self.assertEqual(summary['excluded_frames'],0)
        words[129]^=1
        with self.assertRaises(ValueError): tool.summarize(words,ref,'full-mram',0,0)

    def test_all_flagged_frames_keep_hash_pass_without_inventing_timing(self):
        words,ref=self.valid_run()
        for i in range(tool.HEADER_WORDS,len(words),tool.FRAME_WORDS): words[i+4]=0
        summary=tool.summarize(words,ref,'full-mram',0,0)
        self.assertTrue(summary['valid'])
        self.assertFalse(summary['timing_valid'])
        self.assertEqual(summary['excluded_frames'],160)
        self.assertEqual(summary['warmup_flagged_frames'],64)
        self.assertIsNone(summary['median_ms'])
        self.assertIsNone(summary['median_tick_ms'])
        self.assertTrue(all(row['median_ms'] is None for row in summary['frames']))

    def test_invalid_reference_and_old_protocol_rejected(self):
        words,ref=self.valid_run(); ref['hashes']['0']=[]
        with self.assertRaises(ValueError): tool.summarize(words,ref,'full-mram',0,0)
        words,ref=self.valid_run(); words[0]=0x44530103
        with self.assertRaises(ValueError): tool.summarize(words,ref,'full-mram',0,0)

    def local_transport(self):
        transport=object.__new__(tool.MeasurementBleTransport)
        transport.connect=Mock(); transport.discover=Mock(return_value=True)
        transport.set_notify=Mock(); transport.disconnect=Mock(); transport.notes=queue.Queue()
        characteristic=SimpleNamespace(properties=['write-without-response'],max_write_without_response_size=20)
        transport.client=SimpleNamespace(services=SimpleNamespace(get_characteristic=lambda _: characteristic))
        return transport

    def test_windows_connects_with_bledevice_no_scan_or_uncached_override(self):
        transport=object.__new__(tool.MeasurementBleTransport)
        transport.address='00:00:00:00:00:01'; transport.side='left'
        transport.disconnected_event=Mock(); transport._install_disconnect_error_hook=Mock()
        transport._call=asyncio.run
        bleak_client=Mock(); bleak_client.connect=AsyncMock()
        with patch.object(tool.os,'name','nt'),patch('bleak.BleakClient',return_value=bleak_client) as factory, \
                patch('bleak.BleakScanner.discover') as scan,redirect_stdout(io.StringIO()):
            transport.connect()
        scan.assert_not_called()
        self.assertEqual(factory.call_args.args[0].address,transport.address)
        self.assertEqual(set(factory.call_args.kwargs),{'disconnected_callback'})
        bleak_client.connect.assert_awaited_once_with(timeout=30)

    def test_login_retries_data_ctrl_settle_and_drain(self):
        transport=self.local_transport(); transport.notes.put(('old',b'stale'))
        attempts=[]
        def login(tp):
            self.assertTrue(tp.notes.empty()); attempts.append(1)
            if len(attempts)<3:
                tp.notes.put(('old',b'late')); raise TimeoutError()
        with patch.object(tool,'authenticate',side_effect=login),patch.object(tool.time,'sleep') as sleep, \
                redirect_stdout(io.StringIO()): tool.Client(transport,'left').connect()
        self.assertEqual(len(attempts),3); transport.connect.assert_called_once()
        sleep.assert_called_once_with(2.5)
        self.assertEqual(transport.set_notify.call_args_list[0].args,(tool.DATA[0],tool.DATA[2],True))
        self.assertEqual(transport.set_notify.call_args_list[1].args,(tool.CTRL[0],tool.CTRL[2],True))

    def test_cancel_access_denied_and_incomplete_services_rebuild(self):
        for error in (asyncio.CancelledError(),OSError('services access denied'),TimeoutError()):
            transport=self.local_transport(); transport.connect.side_effect=[error,None]
            with patch.object(tool,'authenticate'),patch.object(tool.time,'sleep'),redirect_stdout(io.StringIO()):
                tool.Client(transport,'left').connect()
            self.assertEqual(transport.connect.call_count,2); transport.disconnect.assert_called_once()
        transport=self.local_transport(); transport.discover.side_effect=[False,True]
        with patch.object(tool,'authenticate'),patch.object(tool.time,'sleep'),redirect_stdout(io.StringIO()):
            tool.Client(transport,'left').connect()
        self.assertEqual(transport.connect.call_count,2)

    def test_three_unanswered_logins_rebuild_connection_bounded(self):
        transport=self.local_transport()
        with patch.object(tool,'authenticate',side_effect=TimeoutError()) as login, \
                patch.object(tool.time,'sleep'),redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError,'three connections'):
                tool.Client(transport,'left').connect()
        self.assertEqual(login.call_count,9); self.assertEqual(transport.connect.call_count,3)
        self.assertEqual(transport.disconnect.call_count,3)

    def test_b_setup_metadata_and_a_firmware_are_not_interchangeable(self):
        for setup in ('small-mram','small-ram','mixed-mram','mixed-ram'):
            words,ref=self.valid_run(); words[0]=tool.B_MAGIC; words[1]=tool.SETUPS[setup]
            self.assertTrue(tool.summarize(words,ref,setup,0,0)['valid'])
            words[0]=tool.MAGIC
            with self.assertRaises(ValueError): tool.summarize(words,ref,setup,0,0)

    def test_new_session_keeps_sequence_but_clears_stale_results(self):
        client=tool.Client(None,'left'); old=client.session; seq=client.next_sequence
        client.results={65535:4}; client.new_session()
        self.assertNotEqual(old,client.session); self.assertFalse(client.results)
        self.assertEqual(client.next_sequence,seq)

    def test_progress_is_timestamped_flushed_and_empty_errors_named(self):
        with patch('builtins.print') as printer:
            tool.progress('sending RUN')
            self.assertTrue(printer.call_args.kwargs['flush'])
            self.assertRegex(printer.call_args.args[0],r'^\[\d{4}-\d{2}-\d{2}T.*\] sending RUN$')
        with patch.object(tool,'run',side_effect=TimeoutError()),redirect_stderr(io.StringIO()) as output:
            self.assertEqual(tool.main(['run','-c','g2://local','--lens','left','--setup','full-mram',
                '--skip-chroma','0','--clip','unused','--reference','unused','--output','unused']),1)
        self.assertIn('TimeoutError: TimeoutError()',output.getvalue())

    def test_completed_run_saves_raw_progress_and_both_timing_sources(self):
        words,ref=self.valid_run()
        client=Mock(); client.results={65535:4}
        client.command.return_value=tool.MAGIC
        client.read.side_effect=lambda i: len(words) if i==3 else words[i]
        client.close_session.return_value=[123]*6
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); (root/'clip').write_bytes(b'fake')
            (root/'ref').write_text(tool.json.dumps(ref),encoding='utf-8')
            args=SimpleNamespace(clip=root/'clip',reference=root/'ref',output=root/'result.json',
                uncached=0,setup='full-mram',skip_chroma=0,
                connection='g2://local?left=00:00:00:00:00:01&right=00:00:00:00:00:02&addressType=public',
                lens='left',mtu=23)
            def command(op,*args,**kwargs):
                if op==tool.RUN: client.results[65535]=4
                return tool.MAGIC
            client.command.side_effect=command
            with patch.object(tool,'check_clip',return_value=tool.PROFILES[0]), \
                    patch.object(tool,'MeasurementBleTransport'),patch.object(tool,'Client',return_value=client), \
                    redirect_stdout(io.StringIO()) as output:
                tool.run(args)
            saved=tool.json.loads((root/'result.json').read_text(encoding='utf-8'))
            self.assertEqual(saved['median_ms'],2)
            self.assertEqual(saved['median_tick_ms'],2)
            self.assertEqual(tool.json.loads((root/'result.raw.json').read_text(encoding='utf-8'))['words'],words)
            self.assertIn('value 1248/1248',output.getvalue())
            self.assertIn('excluded 0/160',output.getvalue())
            client.transport.close.assert_called_once()

    def test_batch_matrix_uses_one_authenticated_connection(self):
        _,ref=self.valid_run(); client=Mock(); client.command.return_value=tool.B_MAGIC
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); (root/'clip').write_bytes(b'fake')
            (root/'ref').write_text(tool.json.dumps(ref),encoding='utf-8')
            args=SimpleNamespace(clip=root/'clip',reference=root/'ref',output_dir=root,
                setups=['small-mram','small-ram','mixed-mram','mixed-ram'],skip_chroma=[0,1],
                connection='g2://local?left=00:00:00:00:00:01&right=00:00:00:00:00:02&addressType=public',lens='left',mtu=23)
            with patch.object(tool,'check_clip',return_value=tool.PROFILES[0]),patch.object(tool,'MeasurementBleTransport'), \
                    patch.object(tool,'Client',return_value=client),patch.object(tool,'run_case',return_value={'valid':True}) as case, \
                    patch.object(tool.time,'sleep'),redirect_stdout(io.StringIO()): tool.batch(args)
            client.connect.assert_called_once(); client.transport.close.assert_called_once()
            self.assertEqual(case.call_count,8); self.assertEqual(client.new_session.call_count,8)
            self.assertEqual([(c.args[1].setup,c.args[1].skip_chroma) for c in case.call_args_list],
                             [(s,c) for s in args.setups for c in (0,1)])
            self.assertEqual(len(tool.json.loads((root/'batch-summary.json').read_text(encoding='utf-8'))),8)

    def test_batch_stops_after_failure_without_reconnecting_or_replaying(self):
        _,ref=self.valid_run(); client=Mock(); client.command.return_value=tool.B_MAGIC
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); (root/'clip').write_bytes(b'fake')
            (root/'ref').write_text(tool.json.dumps(ref),encoding='utf-8')
            args=SimpleNamespace(clip=root/'clip',reference=root/'ref',output_dir=root,
                setups=['small-mram','small-ram'],skip_chroma=[0,1],
                connection='g2://local?left=00:00:00:00:00:01&right=00:00:00:00:00:02&addressType=public',lens='left',mtu=23)
            with patch.object(tool,'check_clip',return_value=tool.PROFILES[0]),patch.object(tool,'MeasurementBleTransport'), \
                    patch.object(tool,'Client',return_value=client),patch.object(tool,'run_case',side_effect=TimeoutError()) as case, \
                    redirect_stdout(io.StringIO()):
                with self.assertRaises(TimeoutError): tool.batch(args)
            case.assert_called_once(); client.connect.assert_called_once(); client.transport.close.assert_called_once()
            self.assertFalse((root/'batch-summary.json').exists())

    def test_batch_preserves_existing_raw_or_summary_without_connecting(self):
        for filename in ('batch-summary.json','deblocking-comparison.json','left-tokyo-original-small-mram-color.raw.json'):
            with tempfile.TemporaryDirectory() as directory:
                root=Path(directory); (root/filename).write_text('old evidence',encoding='utf-8')
                (root/'clip').write_bytes(b'fake')
                args=SimpleNamespace(output_dir=root,setups=['small-mram'],skip_chroma=[0],lens='left',clip=root/'clip',reference=root/'ref')
                with patch.object(tool,'measure') as connect,patch.object(tool,'check_clip',return_value=tool.PROFILES[0]):
                    with self.assertRaises(FileExistsError): tool.batch(args)
                connect.assert_not_called()
                self.assertEqual((root/filename).read_text(encoding='utf-8'),'old evidence')

    def test_bad_clip_rejected_without_connecting(self):
        with self.assertRaises(ValueError): tool.check_clip(b'\0'*tool.CLIP_BYTES)

    def test_deblock_profiles_match_firmware_metadata(self):
        root=Path(__file__).resolve().parents[2]
        text=(root/'patches/decoder_speed/clips.h').read_text(encoding='utf-8')
        self.assertIn(f'#define DS_CLIP_BYTES {tool.CLIP_BYTES}U',text)
        self.assertIn(f'#define DS_CLIP_CRC {tool.CLIP_CRC:#x}U',text)
        pairs=[(int(a),int(b,16)) for a,b in re.findall(r'\{(\d+)U, (0x[0-9a-f]+)U\}',text)]
        self.assertEqual(pairs,[(p['bytes'],p['crc32']) for p in tool.PROFILES[1:]])
        self.assertTrue(all(p['bytes']<=tool.CLIP_BYTES for p in tool.PROFILES))
        self.assertEqual([p['deblocking'] for p in tool.PROFILES],['on','on','off'])

    def test_results_require_the_selected_clip_length_crc_and_y_reference(self):
        for profile in tool.PROFILES:
            words,ref=self.valid_run(); words[0]=tool.B_MAGIC
            words[6]=profile['crc32']; words[14]=profile['bytes']
            ref.update(clip_sha256=profile['sha256'],clip_crc32=profile['crc32'])
            self.assertTrue(tool.summarize(words,ref,'small-mram',0,0)['valid'])
            words[6]^=1
            with self.assertRaisesRegex(ValueError,'metadata invalid'): tool.summarize(words,ref,'small-mram',0,0)

    def test_clip_setup_mode_batch_uses_one_connection_and_distinct_outputs(self):
        client=Mock(); client.command.return_value=tool.B_MAGIC
        _,ref=self.valid_run()
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); clips=[]; refs=[]
            profiles={}
            for index,profile in enumerate(tool.PROFILES):
                clip=root/f'clip-{index}'; clip.write_bytes(bytes([index])); clips.append(clip)
                reference=dict(ref,clip_sha256=profile['sha256'],clip_crc32=profile['crc32'])
                path=root/f'ref-{index}'; path.write_text(tool.json.dumps(reference),encoding='utf-8'); refs.append(path)
                profiles[bytes([index])]=profile
            args=SimpleNamespace(clip=clips,reference=refs,output_dir=root/'results',setups=['small-mram','small-ram','mixed-mram','mixed-ram'],
                skip_chroma=[0,1],connection='g2://local?left=00:00:00:00:00:01&right=00:00:00:00:00:02&addressType=public',lens='left',mtu=23)
            (root/'results').mkdir()
            with patch.object(tool,'check_clip',side_effect=lambda raw: profiles[raw]),patch.object(tool,'MeasurementBleTransport'), \
                    patch.object(tool,'Client',return_value=client),patch.object(tool,'run_case',return_value={'valid':True}) as case, \
                    patch.object(tool.time,'sleep'),redirect_stdout(io.StringIO()): tool.batch(args)
            client.connect.assert_called_once(); self.assertEqual(case.call_count,24)
            self.assertEqual(len({call.args[1].output for call in case.call_args_list}),24)
            self.assertEqual([call.args[1].clip for call in case.call_args_list[:3]],clips)
            self.assertEqual([call.args[3]['name'] for call in case.call_args_list[:3]],[p['name'] for p in tool.PROFILES])
            self.assertEqual(client.new_session.call_count,24); client.transport.close.assert_called_once()

    def test_missing_reference_and_duplicate_clips_stop_before_connecting(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); clip=root/'clip'; clip.write_bytes(b'fake')
            args=SimpleNamespace(output_dir=root,clip=[clip,clip],reference=[root/'ref'],setups=['small-mram'],skip_chroma=[0],lens='left')
            with patch.object(tool,'measure') as connect,patch.object(tool,'check_clip',return_value=tool.PROFILES[0]):
                with self.assertRaisesRegex(ValueError,'one PC reference'): tool.batch(args)
                args.reference=[root/'ref',root/'ref']
                with self.assertRaisesRegex(ValueError,'same clip'): tool.batch(args)
            connect.assert_not_called()

    def test_reference_for_other_clip_and_a_deblock_run_stop_before_connecting(self):
        _,ref=self.valid_run()
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); (root/'clip').write_bytes(b'fake'); (root/'ref').write_text(tool.json.dumps(ref),encoding='utf-8')
            args=SimpleNamespace(clip=root/'clip',reference=root/'ref',setup='small-mram',uncached=0)
            with patch.object(tool,'check_clip',return_value=tool.PROFILES[1]),patch.object(tool,'Client') as client:
                with self.assertRaisesRegex(ValueError,'identity/format'): tool.measure(args,[args])
                ref.update(clip_sha256=tool.PROFILES[1]['sha256'],clip_crc32=tool.PROFILES[1]['crc32'])
                (root/'ref').write_text(tool.json.dumps(ref),encoding='utf-8'); args.setup='full-mram'
                with self.assertRaisesRegex(ValueError,'A accepts only'): tool.measure(args,[args])
            client.assert_not_called()

    def test_deblock_comparison_reports_both_clocks_and_never_invents_invalid_gain(self):
        runs=[dict(setup='small-mram',skip_chroma=True,clip_name='tokyo-deblock-'+mode,
                   timing_valid=True,excluded_frames=i,median_ms=ms,median_tick_ms=ms)
              for i,(mode,ms) in enumerate((('on',60),('off',52)))]
        rows=tool.deblocking_comparisons(runs)
        self.assertEqual(len(rows),1); self.assertTrue(rows[0]['timing_valid'])
        self.assertAlmostEqual(rows[0]['cycles']['saving_percent'],100*8/60)
        self.assertEqual(rows[0]['cycles'],rows[0]['ticks'])
        self.assertEqual((rows[0]['excluded_on'],rows[0]['excluded_off']),(0,1))
        runs[1].update(timing_valid=False,median_ms=None,median_tick_ms=None)
        row=tool.deblocking_comparisons(runs)[0]
        self.assertFalse(row['timing_valid']); self.assertIsNone(row['cycles']['saving_percent'])
        self.assertEqual(tool.deblocking_comparisons(runs[:1]),[])

    def test_client_upload_fragmentation_ack_and_result(self):
        transport=FakeTransport()
        client=tool.Client(transport,'left'); client.max_write=20; client.session=321; client.next_sequence=254
        self.assertEqual(client.command(tool.HELLO,result_index=65535),tool.MAGIC)
        client.command(tool.WRITE,struct.pack('<I',0)+bytes(range(256))*8)
        self.assertEqual(len(transport.received[-1]),2059)
        self.assertEqual(transport.received[-1][11:],bytes(range(256))*8)
        self.assertTrue(all(len(p)<=20 for p in transport.packets))
        self.assertEqual(client.read(17),1234)

    def test_client_nack_never_accepts_stale_result(self):
        transport=FakeTransport(); transport.nack=True
        client=tool.Client(transport,'left'); client.max_write=20; client.session=321
        client.results[17]=1234
        with self.assertRaises(RuntimeError): client.command(tool.READ,struct.pack('<H',17),result_index=17)

    def test_close_retries_park_then_reads_released_heaps(self):
        client=tool.Client(None,'left')
        calls=[]
        def command(op,args=b'',result_index=None):
            calls.append(op)
            if len(calls)==1: raise RuntimeError('not parked')
            return 123 if result_index is not None else None
        client.command=command
        with patch.object(tool.time,'sleep'):
            self.assertEqual(client.close_session(),[123]*6)
        self.assertEqual(calls,[tool.CLOSE,tool.CLOSE]+[tool.HEAPS]*6)


class FakeTransport:
    """Round-trip actual compressed minimum-MTU packets through fake replies."""
    def __init__(self):
        self.notes=queue.Queue(); self.buffer=bytearray(); self.packets=[]; self.received=[]; self.nack=False

    def write(self,service,characteristic,hex_data,mode):
        packet=bytes.fromhex(hex_data); self.packets.append(packet)
        assert tool.crc16(packet[8:-2])==packet[-2:]
        if packet[8]&128:
            self.buffer.clear(); self.stream=packet[2]; self.next=packet[2]
        assert packet[2]==self.next
        self.next=(self.next+1)&255; self.buffer.extend(packet[9:-2])
        if not packet[8]&64: return
        record=bytes(self.buffer)
        assert len(record)==5+int.from_bytes(record[1:3],'little')
        payload=zlib.decompressobj().decompress(record[5:]); self.received.append(payload)
        assert tool.crc16(payload)==record[3:5]
        op=payload[4]; token=int.from_bytes(payload[5:7],'little')
        body=bytes((3 if self.nack else 1,self.stream,0,0,1))+struct.pack('<H',len(payload))+tool.crc16(payload)
        self.notes.put((tool.CTRL[2],result_frame(body)))
        if not self.nack and op in (tool.HELLO,tool.READ):
            index=65535 if op==tool.HELLO else int.from_bytes(payload[7:9],'little')
            self.notes.put((tool.CTRL[2],result_frame(struct.pack('<BBHHI',5,1,token,index,tool.MAGIC if op==tool.HELLO else 1234))))


if __name__=='__main__': unittest.main()
