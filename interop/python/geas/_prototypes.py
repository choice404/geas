"""Function prototypes for the loaded runtime library."""

import ctypes as C

from ._abi import (
    GeasAttrInfo,
    GeasClockFn,
    GeasCourseInfo,
    GeasEventFn,
    GeasInameEntry,
    GeasPledgeFn,
    GeasRef,
    GeasRuntimeConfig,
    GeasSleepFn,
    GeasValue,
    GeasVowBinding,
)


def _declare(lib):
    """Prototypes, each one spelled in docs/abi.md."""
    v, i, z = C.c_void_p, C.c_int, C.c_size_t
    s, VP = C.c_char_p, C.POINTER(GeasValue)
    lib.geas_runtime_init.argtypes = [C.POINTER(GeasRuntimeConfig),
                                     C.POINTER(v)]
    lib.geas_runtime_init.restype = i
    lib.geas_runtime_shutdown.argtypes = [v]
    lib.geas_runtime_shutdown.restype = None
    lib.geas_runtime_set_event_hook.argtypes = [v, GeasEventFn, v]
    lib.geas_runtime_set_event_hook.restype = i
    lib.geas_module_load.argtypes = [v, s]
    lib.geas_module_load.restype = i
    lib.geas_runtime_freeze.argtypes = [v]
    lib.geas_runtime_freeze.restype = i
    lib.geas_pledge_bind.argtypes = [v, s, GeasPledgeFn]
    lib.geas_pledge_bind.restype = i
    lib.geas_contract_sign.argtypes = [v, s, C.POINTER(GeasVowBinding), z,
                                      C.c_uint64, C.POINTER(v)]
    lib.geas_contract_sign.restype = i
    lib.geas_instance_park.argtypes = [v, s, s]
    lib.geas_instance_park.restype = i
    lib.geas_instance_resume.argtypes = [v, s, s, C.c_uint64,
                                        C.POINTER(v)]
    lib.geas_instance_resume.restype = i
    lib.geas_instance_claim.argtypes = [v, s, s, C.c_uint64, C.POINTER(v)]
    lib.geas_instance_claim.restype = i
    lib.geas_contract_state.argtypes = [v]
    lib.geas_contract_state.restype = i
    lib.geas_contract_hash.argtypes = [v]
    lib.geas_contract_hash.restype = C.c_uint64
    lib.geas_contract_signed_at.argtypes = [v]
    lib.geas_contract_signed_at.restype = C.c_int64
    lib.geas_contract_amended_from.argtypes = [v]
    lib.geas_contract_amended_from.restype = C.c_uint32
    lib.geas_contract_break.argtypes = [v]
    lib.geas_contract_break.restype = i
    lib.geas_cancel_requested.argtypes = [v]
    lib.geas_cancel_requested.restype = i
    lib.geas_contract_request_cancel.argtypes = [v]
    lib.geas_contract_request_cancel.restype = i
    lib.geas_contract_release.argtypes = [v]
    lib.geas_contract_release.restype = i
    lib.geas_pledge_fulfill.argtypes = [v, s, VP, z, C.POINTER(GeasRef), z]
    lib.geas_pledge_fulfill.restype = v
    lib.geas_future_wait.argtypes = [v, VP]
    lib.geas_future_wait.restype = i
    lib.geas_future_poll.argtypes = [v, C.POINTER(i)]
    lib.geas_future_poll.restype = i
    lib.geas_future_release.argtypes = [v]
    lib.geas_future_release.restype = i
    lib.geas_pledge_fulfill_sync.argtypes = [v, s, VP, z,
                                            C.POINTER(GeasRef), z, VP]
    lib.geas_pledge_fulfill_sync.restype = i
    lib.geas_vow_ref.argtypes = [v, s]
    lib.geas_vow_ref.restype = VP
    lib.geas_partial_count.argtypes = [v, i]
    lib.geas_partial_count.restype = z
    lib.geas_partial_name.argtypes = [v, i, z]
    lib.geas_partial_name.restype = s
    lib.geas_partial_nerrors.argtypes = [v]
    lib.geas_partial_nerrors.restype = z
    lib.geas_partial_error.argtypes = [v, z, C.POINTER(s), C.POINTER(VP)]
    lib.geas_partial_error.restype = i
    lib.geas_attr_count.argtypes = [v, s, C.POINTER(z)]
    lib.geas_attr_count.restype = i
    lib.geas_attr_at.argtypes = [v, s, z, C.POINTER(GeasAttrInfo)]
    lib.geas_attr_at.restype = i
    lib.geas_contract_attr.argtypes = [v, s, s, C.POINTER(VP)]
    lib.geas_contract_attr.restype = i
    lib.geas_item_attr.argtypes = [v, s, s, s, C.POINTER(VP)]
    lib.geas_item_attr.restype = i
    lib.geas_instance_attr_count.argtypes = [v, C.POINTER(z)]
    lib.geas_instance_attr_count.restype = i
    lib.geas_instance_attr_at.argtypes = [v, z, C.POINTER(GeasAttrInfo)]
    lib.geas_instance_attr_at.restype = i
    lib.geas_instance_attr.argtypes = [v, s, s, C.POINTER(VP)]
    lib.geas_instance_attr.restype = i
    lib.geas_iname_lookup.argtypes = [v, s, C.POINTER(GeasInameEntry)]
    lib.geas_iname_lookup.restype = i
    lib.geas_iname_count.argtypes = [v]
    lib.geas_iname_count.restype = z
    lib.geas_iname_at.argtypes = [v, z, C.POINTER(GeasInameEntry)]
    lib.geas_iname_at.restype = i
    lib.geas_iname_dump.argtypes = [v, C.c_char_p, z, C.POINTER(z)]
    lib.geas_iname_dump.restype = i
    lib.geas_box.argtypes = [v]
    lib.geas_box.restype = VP
    lib.geas_string_copy.argtypes = [v, s, C.c_uint64]
    lib.geas_string_copy.restype = GeasValue
    lib.geas_bytes_copy_into.argtypes = [v, s, C.c_uint64, VP]
    lib.geas_bytes_copy_into.restype = i
    lib.geas_list_new.argtypes = [v, C.c_uint32, C.c_uint64, VP]
    lib.geas_list_new.restype = i
    lib.geas_list_push.argtypes = [v, VP, VP]
    lib.geas_list_push.restype = i
    lib.geas_bytes.argtypes = [v, C.c_uint64]
    lib.geas_bytes.restype = C.c_void_p
    lib.geas_map_new_into.argtypes = [v, C.c_uint32, VP]
    lib.geas_map_new_into.restype = i
    lib.geas_map_set.argtypes = [v, VP, VP, VP]
    lib.geas_map_set.restype = i
    # The workflow surface: signals, the clock and the tick, lapses, and
    # the remedy partial surface
    lib.geas_signal.argtypes = [v, s, VP, z]
    lib.geas_signal.restype = i
    lib.geas_signal_payload.argtypes = [v, s, C.POINTER(VP)]
    lib.geas_signal_payload.restype = i
    lib.geas_runtime_set_clock.argtypes = [v, GeasClockFn, GeasSleepFn, v]
    lib.geas_runtime_set_clock.restype = i
    lib.geas_instance_tick.argtypes = [v]
    lib.geas_instance_tick.restype = i
    lib.geas_runtime_tick.argtypes = [v]
    lib.geas_runtime_tick.restype = i
    lib.geas_partial_lapsed.argtypes = [v, s]
    lib.geas_partial_lapsed.restype = C.c_int
    lib.geas_contract_term_lapsed.argtypes = [v]
    lib.geas_contract_term_lapsed.restype = C.c_int
    lib.geas_remedy_state.argtypes = [v, s]
    lib.geas_remedy_state.restype = C.c_int
    lib.geas_partial_remedy_count.argtypes = [v]
    lib.geas_partial_remedy_count.restype = z
    lib.geas_partial_remedy_name.argtypes = [v, z]
    lib.geas_partial_remedy_name.restype = s
    lib.geas_partial_remedy_status.argtypes = [v, z]
    lib.geas_partial_remedy_status.restype = i
    lib.geas_partial_remedy_lapsed.argtypes = [v, z]
    lib.geas_partial_remedy_lapsed.restype = C.c_int
    lib.geas_partial_remedy_error.argtypes = [v, z, C.POINTER(VP)]
    lib.geas_partial_remedy_error.restype = i
    # The course surface: the runtime's ticker, where a course stands,
    # the wait on it, and the park row an instance keeps writing itself
    lib.geas_runtime_start_ticker.argtypes = [v, C.c_uint32]
    lib.geas_runtime_start_ticker.restype = i
    lib.geas_runtime_stop_ticker.argtypes = [v]
    lib.geas_runtime_stop_ticker.restype = i
    lib.geas_course_info.argtypes = [v, C.POINTER(GeasCourseInfo)]
    lib.geas_course_info.restype = i
    lib.geas_course_wait.argtypes = [v, C.c_int64, C.POINTER(GeasCourseInfo)]
    lib.geas_course_wait.restype = i
    lib.geas_contract_set_autopark.argtypes = [v, s, s]
    lib.geas_contract_set_autopark.restype = i
