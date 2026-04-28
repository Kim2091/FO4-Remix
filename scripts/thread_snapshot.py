"""
Quick thread-state snapshot of a running process via Frida.

Lists all threads, their current PC, and resolves the PC to module+offset.
Useful for diagnosing hangs: tells you exactly what each thread is parked on.

Usage: python thread_snapshot.py <pid>
"""
import frida
import sys

js = """
rpc.exports = {
    snapshot: function () {
        var threads = Process.enumerateThreads();
        var out = [];
        for (var i = 0; i < threads.length; i++) {
            var t = threads[i];
            var pc = t.context.pc || t.context.rip;
            var entry = {
                id: t.id,
                state: t.state,
                pc: pc.toString(),
                module: '?',
                modOff: '?',
                symbol: '?'
            };
            try {
                var sym = DebugSymbol.fromAddress(pc);
                if (sym) {
                    entry.module = sym.moduleName || '?';
                    entry.symbol = sym.name || '?';
                }
                var mod = Process.findModuleByAddress(pc);
                if (mod) {
                    entry.module = mod.name;
                    entry.modOff = '0x' + pc.sub(mod.base).toString(16);
                }
            } catch (e) {}
            // Try a fuzzy backtrace if thread is known-suspended-ish
            try {
                var bt = Thread.backtrace(t.context, Backtracer.FUZZY);
                entry.bt = bt.slice(0, 12).map(function (a) {
                    var m = Process.findModuleByAddress(a);
                    if (m) return m.name + '+0x' + a.sub(m.base).toString(16);
                    return a.toString();
                });
            } catch (e) {
                entry.bt = ['<bt error: ' + e.message + '>'];
            }
            out.push(entry);
        }
        return out;
    }
};
"""

if len(sys.argv) < 2:
    print("Usage: thread_snapshot.py <pid>")
    sys.exit(1)
pid = int(sys.argv[1])
session = frida.attach(pid)
script = session.create_script(js)
script.load()
result = script.exports_sync.snapshot()

for t in result:
    print(f"\n=== Thread {t['id']} state={t['state']} ===")
    print(f"  PC={t['pc']}  ({t['module']}+{t['modOff']})  {t['symbol']}")
    print(f"  Backtrace:")
    for f in t.get('bt', []):
        print(f"    {f}")

session.detach()
