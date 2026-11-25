import java.io.*;
import xeij.EFPBox;
public class H2 {
  public static void main(String[] a) throws Exception {
    xeij.XEiJ.fmtInit(); EFPBox box = new EFPBox(); box.epbSetMC68882();
    BufferedReader r = new BufferedReader(new InputStreamReader(System.in)); String line;
    while ((line = r.readLine()) != null) {
      String[] t = line.trim().split(" ");
      box.epbSetRoundingPrec(0); box.epbSetRoundingMode(Integer.parseInt(t[1]));
      box.epbFpsr = 0;
      if (t[0].equals("pout")) {
        EFPBox.EFP x = box.new EFP();
        x.setx012(Integer.parseUnsignedInt(t[3], 16) << 16, Long.parseUnsignedLong(t[4], 16));
        byte[] b = new byte[12]; x.getp012(b, 0, Integer.parseInt(t[2]));
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < 12; i++) s.append(String.format("%02x", b[i] & 255));
        System.out.printf("pout %s %08x%n", s, box.epbFpsr);
      } else {
        EFPBox.EFP y = box.new EFP();
        y.setp012(Integer.parseUnsignedInt(t[2], 16), (Long.parseUnsignedLong(t[3], 16) << 32) | Long.parseUnsignedLong(t[4], 16));
        byte[] b = new byte[12]; y.getx012(b, 0);
        long sig = 0; for (int i = 4; i < 12; i++) sig = sig << 8 | (b[i] & 255);
        System.out.printf("pin %04x %016x %08x%n", ((b[0] & 255) << 8) | (b[1] & 255), sig, box.epbFpsr);
      }
    }
  }
}
