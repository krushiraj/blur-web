import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.*;
public class ExportFuncs extends GhidraScript {
  public void run() throws Exception {
    try (PrintWriter w = new PrintWriter(getScriptArgs()[0])) {
      FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
      while (it.hasNext()) {
        Function f = it.next(); AddressSetView b = f.getBody();
        long lo = b.getMinAddress().getOffset(), hi = b.getMaxAddress().getOffset();
        w.println(String.format("%08x\t%d\t%08x\t%d\t%d", f.getEntryPoint().getOffset(), b.getNumAddresses(), hi, b.getNumAddressRanges(), f.isThunk() ? 1 : 0));
      }
    }
  }
}
