"""Generated DEngine debugger support; no target expressions or function calls."""
import lldb


def _value(value):
    if not value.IsValid() or value.GetError().Fail():
        return "?"
    if value.GetType().GetBasicType() in (lldb.eBasicTypeFloat, lldb.eBasicTypeDouble):
        try:
            return "%g" % float(value.GetValue())
        except (TypeError, ValueError):
            return "?"
    return (value.GetSummary() or value.GetValue() or
            ("[%d elements]" % value.GetNumChildren()))[:256]


def reflected(value, _dict):
    value = value.GetNonSyntheticValue()
    name = value.GetType().GetCanonicalType().GetName()
    info = TYPES.get(name) or TYPES.get(value.GetTypeName())
    if info is None:
        return None
    size, fields = info
    if value.GetByteSize() != size:
        return "<reflection layout changed; regenerate visualizers>"
    parts = [field + "=" + _value(value.GetChildMemberWithName(field))
             for field in fields[:6]]
    if len(fields) > 6:
        parts.append("…")
    return name + " {" + ", ".join(parts) + "}"


def string(value, _dict):
    value = value.GetNonSyntheticValue()
    n = value.GetChildMemberWithName("len").GetValueAsUnsigned(0)
    address = value.GetChildMemberWithName("ptr").GetValueAsUnsigned(0)
    if n == 0:
        return '""'
    if address == 0:
        return "<invalid string>"
    error = lldb.SBError()
    data = value.GetProcess().ReadMemory(address, min(n, 4096), error)
    if error.Fail():
        return "<unreadable string>"
    # repr escapes embedded newlines/NUL rather than corrupting the debugger UI.
    return repr(data.decode("utf-8", "replace")) + ("…" if n > 4096 else "")


def container(value, _dict):
    value = value.GetNonSyntheticValue()
    count = value.GetChildMemberWithName("count").GetValueAsUnsigned(0)
    capacity = value.GetChildMemberWithName("capacity").GetValueAsUnsigned(0)
    return "%d elements, capacity %d%s" % (count, capacity,
                                           " (invalid)" if count > capacity else "")


def entity(value, _dict):
    bits = value.GetNonSyntheticValue().GetChildMemberWithName("bits").GetValueAsUnsigned(0)
    return "null entity" if bits == 0 else "entity %d gen %d" % (bits & 0xffffffff, bits >> 32)


def matrix(value, _dict):
    cells = value.GetNonSyntheticValue().GetChildMemberWithName("m")
    count = cells.GetNumChildren()
    rows, columns = (3, 3) if count == 9 else (3, 4) if count == 12 else (4, 4)
    if count not in (9, 12, 16):
        return "<matrix layout changed; regenerate visualizers>"
    return "[" + " ".join("(" + ", ".join(_value(cells.GetChildAtIndex(c * rows + r))
                                            for c in range(columns)) + ")"
                          for r in range(rows)) + "]"


def __lldb_init_module(debugger, _dict):
    # Explicit names avoid regex overreach and do not change unrelated C types.
    for name in TYPES:
        debugger.HandleCommand("type summary add -F %s.reflected '%s'" % (__name__, name))
    for name, fn in (("de_str", "string"), ("de_array", "container"),
                     ("de_hashmap", "container"), ("de_entity", "entity"),
                     ("de_mat3", "matrix"), ("de_mat4", "matrix"), ("de_affine", "matrix")):
        debugger.HandleCommand("type summary add -F %s.%s '%s'" % (__name__, fn, name))


# Appended by de_debug_visualizers_write from the real registry.
