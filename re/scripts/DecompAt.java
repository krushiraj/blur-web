import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
public class DecompAt extends GhidraScript {
  public void run() throws Exception {
    DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
    for (String a : getScriptArgs()) {
      Address ad = toAddr(Long.parseLong(a, 16));
      Function f = getFunctionContaining(ad);
      println("=== " + a + " -> " + (f==null? "none" : f.getName()+" @ "+f.getEntryPoint()+" size "+f.getBody().getNumAddresses()));
      if (f != null) {
        println("callers:");
        for (Reference r : getReferencesTo(f.getEntryPoint())) println("  " + r.getFromAddress());
        DecompileResults r = d.decompileFunction(f, 60, monitor);
        if (r.decompileCompleted()) println(r.getDecompiledFunction().getC());
      }
    }
  }
}
