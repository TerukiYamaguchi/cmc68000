import java.io.*;
import xeij.EFPBox;
public class Harness {
  public static void main(String[] a) throws Exception {
    EFPBox box = new EFPBox();
    box.epbSetMC68882();
    BufferedReader r = new BufferedReader(new InputStreamReader(System.in));
    String line;
    while ((line = r.readLine()) != null) {
      String[] t = line.trim().split(" ");
      String fn = t[0];
      int prec = 0;
      if (fn.contains("@")) { prec = Integer.parseInt(fn.substring(fn.indexOf('@') + 1)); fn = fn.substring(0, fn.indexOf('@')); }
      int mode = Integer.parseInt(t[1]);
      box.epbSetRoundingPrec(prec);
      box.epbSetRoundingMode(mode);
      if (fn.equals("movecr")) {
        EFPBox.EFP m = box.new EFP();
        box.epbFpsr = 0;
        box.epbFmovecr(m, Integer.parseInt(t[2], 16));
        byte[] mb = new byte[12]; m.getx012(mb, 0);
        long msig = 0; for (int i = 4; i < 12; i++) msig = msig << 8 | (mb[i] & 255);
        System.out.printf("%s %04x %016x %08x%n", fn, ((mb[0] & 255) << 8) | (mb[1] & 255), msig, box.epbFpsr);
        continue;
      }
      EFPBox.EFP x = box.new EFP();
      x.setx012(Integer.parseUnsignedInt(t[2], 16) << 16, Long.parseUnsignedLong(t[3], 16));
      EFPBox.EFP y = box.new EFP();
      EFPBox.EFP z = null;
      if (t.length > 4) { z = box.new EFP(); z.setx012(Integer.parseUnsignedInt(t[4], 16) << 16, Long.parseUnsignedLong(t[5], 16)); }
      box.epbFpsr = 0;
      switch (fn) {
        case "sin": y.sin(x); break; case "cos": y.cos(x); break; case "tan": y.tan(x); break;
        case "atan": y.atan(x); break; case "asin": y.asin(x); break; case "acos": y.acos(x); break;
        case "sinh": y.sinh(x); break; case "cosh": y.cosh(x); break; case "tanh": y.tanh(x); break; case "atanh": y.atanh(x); break;
        case "etox": y.exp(x); break; case "logn": y.log(x); break; case "log2": y.log2(x); break; case "log10": y.log10(x); break;
        case "etoxm1": y.expm1(x); break; case "lognp1": y.log1p(x); break; case "twotox": y.exp2(x); break; case "tentox": y.exp10(x); break;
        case "sqrt": y.sqrt(x); break;
        case "getexp": y.getexp(x); break; case "getman": y.getman(x); break;
        case "sglmul": {
          EFPBox.EFP a2 = box.new EFP(), b2 = box.new EFP();
          int sr = box.epbFpsr;
          a2.roundmanf(x, 1); b2.roundmanf(z, 1);
          box.epbFpsr = sr;
          box.epbSetRoundingPrec(5);
          y.mul(a2, b2);
          break;
        }
        case "sgldiv": box.epbSetRoundingPrec(5); y.div(x, z); break;
        case "sgldivt": {                       // 入力を切り詰めてから割る版（本実装の O6 の規則。XEiJ の FSGLDIV は切り詰めない）
          EFPBox.EFP a2 = box.new EFP(), b2 = box.new EFP();
          int sr = box.epbFpsr;
          a2.roundmanf(x, 1); b2.roundmanf(z, 1);
          box.epbFpsr = sr;
          box.epbSetRoundingPrec(5);
          y.div(a2, b2);
          break;
        }
        case "rem": y.ieeerem(x, z); break; case "mod": y.rem(x, z); break; case "scale": y.scale(x, z); break;
        default: System.out.println("?"); continue;
      }
      byte[] b = new byte[12];
      y.getx012(b, 0);
      long sig = 0; for (int i = 4; i < 12; i++) sig = sig << 8 | (b[i] & 255);
      System.out.printf("%s %04x %016x %08x%n", fn, ((b[0] & 255) << 8) | (b[1] & 255), sig, box.epbFpsr);
    }
  }
}
