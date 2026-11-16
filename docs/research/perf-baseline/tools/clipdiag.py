import ctypes, ctypes.wintypes as w, json, sys, time
u = ctypes.WinDLL("user32", use_last_error=True); k = ctypes.windll.kernel32
u.GetClipboardSequenceNumber.restype = w.DWORD
u.GetProcessWindowStation.restype = w.HANDLE
u.GetUserObjectInformationW.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD)]
buf = ctypes.create_unicode_buffer(256); n = w.DWORD()
u.GetUserObjectInformationW(u.GetProcessWindowStation(), 2, buf, 512, ctypes.byref(n))
sid = w.DWORD(); k.ProcessIdToSessionId(k.GetCurrentProcessId(), ctypes.byref(sid))
seq = u.GetClipboardSequenceNumber(); e1 = ctypes.get_last_error()
ok = u.OpenClipboard(None); e2 = ctypes.get_last_error()
fmt = u.CountClipboardFormats() if ok else -1
if ok: u.CloseClipboard()
json.dump({"winsta": buf.value, "session": sid.value, "seq": seq, "seq_err": e1, "open": ok, "open_err": e2, "formats": fmt, "py": sys.executable}, open(sys.argv[1], "w"))
