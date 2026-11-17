"""Byte buffers through Python arguments, returns, records, vows, and signals."""
import os
import tempfile

from geas import Runtime, Ok, Err, Some, NONE, Record, GeasError, GEAS_ERR_UNBOUND


def main():
    raw = b"\x00\xff\x80A\x00"
    undos = []
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "bytes.db")
        with Runtime("target/geas-out/libgeasrt.so") as rt:
            rt.load("target/geas-out/libblob.geas.so")
            rt.bind("Blob.host", lambda instance, args: Ok(bytearray(args[0] + raw)))
            rt.bind("Blob.undo", lambda instance, args: undos.append(args[0]))
            try:
                rt.sign("Blob")
            except GeasError as error:
                assert error.status == GEAS_ERR_UNBOUND
            else:
                raise AssertionError("Bytes vow must be supplied")
            instance = rt.sign("Blob", {"seed": bytearray(raw)})
            assert instance.vow("seed") == raw
            assert instance.fulfill_sync("raw", bytearray(raw)) == raw
            assert instance.fulfill_sync("echo", raw) == Ok(raw)
            assert instance.fulfill("echo", bytearray(raw)).wait() == Ok(raw)
            assert instance.fulfill_sync("host", raw) == Ok(raw + raw)
            assert instance.fulfill_sync("join", raw, bytearray(raw)) == Ok(raw + raw)
            assert instance.fulfill_sync("count", raw) == Ok(len(raw))
            assert instance.fulfill_sync("equal", raw, raw) == Ok(True)
            assert instance.fulfill_sync("equal", raw, b"") == Ok(False)
            assert instance.fulfill_sync("at", raw, 1) == Ok(255)
            assert instance.fulfill_sync("encode", "a\0é") == Ok("a\0é".encode())
            assert instance.fulfill_sync("decode", "a\0é".encode()) == Some("a\0é")
            assert instance.fulfill_sync("decode", raw) is NONE
            assert instance.fulfill_sync("decode", b"") == Some("")
            assert instance.fulfill_sync("list", raw) == Ok(list(raw))
            assert instance.fulfill_sync("echo_record", Record(bytearray(raw))) == Ok([raw])
            instance.signal("received", bytearray(raw))
            assert instance.signal_payload("received") == raw
            assert instance.fulfill_sync("failure", raw) == Err(raw)
            assert instance.fulfill_sync("echo", bytes(range(256)) * 4096) == Ok(bytes(range(256)) * 4096)
            instance.park(path, "blob")
        undos.clear()
        with Runtime("target/geas-out/libgeasrt.so") as rt:
            rt.load("target/geas-out/libblob.geas.so")
            rt.bind("Blob.host", lambda instance, args: Ok(args[0]))
            rt.bind("Blob.undo", lambda instance, args: undos.append(args[0]))
            resumed = rt.resume(path, "blob")
            assert resumed.vow("seed") == raw
            assert resumed.signal_payload("received") == raw
            assert resumed.partial().errors == [("failure", raw)]
            resumed.break_()
            assert undos == [raw]
    print("[bytes-python] passed")


if __name__ == "__main__":
    main()
