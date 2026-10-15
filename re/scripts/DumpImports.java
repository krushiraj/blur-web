import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class DumpImports extends GhidraScript {
  public void run() throws Exception {
    try (PrintWriter w = new PrintWriter(getScriptArgs()[0])) {
      ExternalManager em = currentProgram.getExternalManager();
      for (String lib : em.getExternalLibraryNames()) {
        ExternalLocationIterator it = em.getExternalLocations(lib);
        while (it.hasNext()) {
          ExternalLocation l = it.next();
          Function f = l.getFunction();
          String cc = "?"; int np = -1; String sig = "";
          if (f != null) { cc = String.valueOf(f.getCallingConventionName()); np = f.getParameterCount(); sig = f.getPrototypeString(false,false); }
          w.println(lib + "\t" + l.getLabel() + "\t" + cc + "\t" + np + "\t" + sig);
        }
      }
    }
  }
}
