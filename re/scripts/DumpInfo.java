import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class DumpInfo extends GhidraScript {
  public void run() throws Exception {
    String out = getScriptArgs()[0];
    try (PrintWriter w = new PrintWriter(out + "/imports.txt")) {
      SymbolIterator it = currentProgram.getSymbolTable().getExternalSymbols();
      while (it.hasNext()) { Symbol s = it.next(); w.println(s.getParentNamespace().getName() + "!" + s.getName()); }
    }
    int n = 0, named = 0;
    FunctionIterator fi = currentProgram.getFunctionManager().getFunctions(true);
    while (fi.hasNext()) { Function f = fi.next(); n++; if (!f.getName().startsWith("FUN_")) named++; }
    try (PrintWriter w = new PrintWriter(out + "/summary.txt")) { w.println("functions=" + n + " named=" + named); }
  }
}
