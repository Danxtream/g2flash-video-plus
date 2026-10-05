"""PC protocol framing and correctness checks; no Bluetooth connection."""
from pathlib import Path
import struct
import sys
import unittest
import queue
import zlib
from unittest.mock import patch

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
        self.assertEqual(tool.message(tool.READ,321,struct.pack('<H',17)),b'\x1fDS\x01\x05A\x01\x11\x00')
        with self.assertRaises(ValueError): tool.message(tool.RUN,65536)

    def valid_run(self):
        words=[0]*(tool.HEADER_WORDS+tool.FRAMES*(tool.PASSES+tool.WARMUPS)*4)
        words[:7]=[tool.MAGIC,0,0,len(words),32,5,tool.CLIP_CRC]
        words[37]=2
        words[12:15]=[320,192,tool.CLIP_BYTES]
        hashes=list(range(32))
        reference=dict(clip_sha256=tool.CLIP_SHA256,decoder='59c66b1',hashes={'0':hashes,'1':hashes})
        for repeat in range(7):
            for frame in range(32):
                i=128+(repeat*32+frame)*4
                ms=10 if repeat<2 else 2
                words[i:i+4]=[250000*ms,frame,ms,250000]
        return words,reference

    def test_warmup_hashes_checked_but_excluded_from_timing(self):
        words,ref=self.valid_run()
        summary=tool.summarize(words,ref,'full-mram',0,0)
        self.assertEqual(summary['median_ms'],2)
        self.assertEqual(len(summary['samples']),160)

    def test_any_warmup_or_measured_mismatch_voids_entire_run(self):
        for repeat in (0,1,2,6):
            words,ref=self.valid_run(); words[128+repeat*128+1]^=1
            with self.assertRaises(ValueError): tool.summarize(words,ref,'full-mram',0,0)

    def test_bad_clock_status_dimensions_setup_and_bounds_rejected(self):
        for index,value in ((0,0),(1,1),(2,13),(3,0),(4,31),(5,4),(6,0),(37,0),(131,0),(128,0),(130,2001)):
            words,ref=self.valid_run(); words[index]=value
            with self.assertRaises(ValueError): tool.summarize(words,ref,'full-mram',0,0)

    def test_bad_clip_rejected_without_connecting(self):
        with self.assertRaises(ValueError): tool.check_clip(b'\0'*tool.CLIP_BYTES)

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
