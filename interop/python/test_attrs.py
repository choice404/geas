"""Declared settings through the Python host surface."""
from geas import Runtime, GeasError, GEAS_ERR_NAME


def unknown(call):
    try:
        call()
    except GeasError as error:
        assert error.status == GEAS_ERR_NAME
    else:
        raise AssertionError("unknown name was accepted")


def main():
    with Runtime("target/geas-out/libgeasrt.so") as rt:
        rt.load("target/geas-out/libjobstep.geas.so")
        rows = rt.attrs("Job")
        assert len(rows) == 12
        assert {row["owner"] for row in rows} == {
            "contract", "pledge", "subcontract", "signal", "remedy"}
        expected = {
            (None, "queue"): "batch", (None, "priority"): -2,
            (None, "weight"): -1.5, (None, "enabled"): True,
            (None, "delay"): -250, ("copy", "program"): "/bin/copy",
            ("copy", "region"): 64 * 1024 * 1024,
            ("copy", "region_max"): 512 * 1024 * 1024,
            ("copy", "threads"): 1, ("Finish", "pool"): "local",
            ("ready", "channel"): "job.ready", ("undo", "program"): "/bin/remove",
        }
        for (item, key), value in expected.items():
            assert rt.attr("Job", key, item) == value
        assert rt.attr("Job", "absent") is None
        assert rt.attr("Job", "absent", "copy") is None
        unknown(lambda: rt.attrs("missing"))
        unknown(lambda: rt.attr("missing", "queue"))
        unknown(lambda: rt.attr("Job", "queue", "missing"))
        contract = rt.sign("Job")
        assert contract.attrs() == rows
        for (item, key), value in expected.items():
            assert contract.attr(key, item) == value
            assert rt.attr("Job", key, item) == value
        assert contract.attr("absent") is None
        unknown(lambda: contract.attr("queue", "missing"))
        contract.break_()
        assert contract.attrs() == rows
    assert rows[0]["value"] == "batch"
    print("[attrs-python] passed")


if __name__ == "__main__":
    main()
