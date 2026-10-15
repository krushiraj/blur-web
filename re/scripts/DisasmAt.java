import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
public class DisasmAt extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    Address s = toAddr(Long.parseLong(a[0], 16)); int n = Integer.parseInt(a[1]);
    Instruction i = getInstructionContaining(s); if (i == null) { println("no instr"); return; }
    Function f = getFunctionContaining(s); println("func: " + (f==null?"none":f.getName()+" @ "+f.getEntryPoint()));
    for (int k = 0; k < n && i != null; k++) { println(i.getAddress() + "  " + i.toString()); i = i.getNext(); }
  }
}
