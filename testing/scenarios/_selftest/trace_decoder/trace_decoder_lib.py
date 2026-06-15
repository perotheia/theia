"""Hermetic Robot library for the trace_decoder selftest.

Wraps :class:`rf_theia.adapters.trace_decoder.TraceDecoder` in Robot-
friendly keywords. Only used by trace_decoder_selftest.robot — this
isn't a user-facing keyword surface.
"""
from __future__ import annotations

from robot.api.deco import keyword, library

from rf_theia.adapters.trace_decoder import TraceDecoder, TraceDecodeError


@library(scope="SUITE")
class TraceDecoderLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        self._dec: TraceDecoder | None = None

    @keyword("Open Trace Decoder")
    def open_trace_decoder(self) -> None:
        """Locate + dlopen libtrace_decoder_ctypes.so."""
        self._dec = TraceDecoder()

    @keyword("Trace Decoder Registered Count")
    def registered_count(self) -> int:
        assert self._dec is not None, "call `Open Trace Decoder` first"
        return self._dec.registered_count()

    @keyword("Decode Trace Payload Hex")
    def decode_hex(self, msg_type_name: str, payload_hex: str) -> dict:
        """Hex-encoded payload → decoded dict. Mirrors what a real
        consumer does with a tracer_jsonl.TraceRecord.payload_hex."""
        assert self._dec is not None, "call `Open Trace Decoder` first"
        return self._dec.decode_hex(msg_type_name, payload_hex)

    @keyword("Decode Trace Payload Hex Expecting Error")
    def decode_hex_error(self, msg_type_name: str, payload_hex: str) -> str:
        """Counterpart for the negative-path tests: asserts the call
        raises and returns the error string."""
        assert self._dec is not None, "call `Open Trace Decoder` first"
        try:
            self._dec.decode_hex(msg_type_name, payload_hex)
        except TraceDecodeError as e:
            return str(e)
        raise AssertionError(
            f"decode_hex({msg_type_name!r}) succeeded but was expected to fail"
        )
