$src = @"
using System;
using System.Text;
using System.Collections.Generic;
public static class Kana {
  // base map for U+FF61..U+FF9F -> fullwidth code points
  static int[] BASE = new int[] {
    0x3002,0x300C,0x300D,0x3001,0x30FB,0x30F2,0x30A1,0x30A3,0x30A5,0x30A7,0x30A9, // FF61-FF6B
    0x30E3,0x30E5,0x30E7,0x30C3,0x30FC, // FF6C-FF70
    0x30A2,0x30A4,0x30A6,0x30A8,0x30AA, // a i u e o  FF71-FF75
    0x30AB,0x30AD,0x30AF,0x30B1,0x30B3, // ka ki ku ke ko
    0x30B5,0x30B7,0x30B9,0x30BB,0x30BD, // sa shi su se so
    0x30BF,0x30C1,0x30C4,0x30C6,0x30C8, // ta chi tsu te to
    0x30CA,0x30CB,0x30CC,0x30CD,0x30CE, // na ni nu ne no
    0x30CF,0x30D2,0x30D5,0x30D8,0x30DB, // ha hi fu he ho
    0x30DE,0x30DF,0x30E0,0x30E1,0x30E2, // ma mi mu me mo
    0x30E4,0x30E6,0x30E8,             // ya yu yo
    0x30E9,0x30EA,0x30EB,0x30EC,0x30ED, // ra ri ru re ro
    0x30EF,0x30F3,                     // wa n
    0x309B,0x309C                      // dakuten, handakuten (standalone)
  };
  static HashSet<int> Voicable = new HashSet<int>(new int[]{
    0x30AB,0x30AD,0x30AF,0x30B1,0x30B3,0x30B5,0x30B7,0x30B9,0x30BB,0x30BD,
    0x30BF,0x30C1,0x30C4,0x30C6,0x30C8,0x30CF,0x30D2,0x30D5,0x30D8,0x30DB});
  static HashSet<int> SemiVoicable = new HashSet<int>(new int[]{0x30CF,0x30D2,0x30D5,0x30D8,0x30DB});

  public static string Convert(string s, out int count){
    count=0;
    var sb=new StringBuilder(s.Length);
    for(int i=0;i<s.Length;i++){
      int c=s[i];
      if(c>=0xFF61 && c<=0xFF9F){
        int b=BASE[c-0xFF61];
        int next = (i+1<s.Length) ? s[i+1] : 0;
        if(next==0xFF9E){ // dakuten
          if(b==0x30A6){ b=0x30F4; i++; }        // u -> vu
          else if(Voicable.Contains(b)){ b=b+1; i++; }
        } else if(next==0xFF9F){ // handakuten
          if(SemiVoicable.Contains(b)){ b=b+2; i++; }
        }
        sb.Append((char)b);
        count++;
      } else {
        sb.Append((char)c);
      }
    }
    return sb.ToString();
  }
}
"@
Add-Type -TypeDefinition $src | Out-Null
$rc = Join-Path (Resolve-Path (Join-Path $PSScriptRoot "..\..")) "porting\StirHex\res\StirHex.rc"
$text = [IO.File]::ReadAllText($rc, [Text.Encoding]::UTF8)
$cnt = 0
$conv = [Kana]::Convert($text, [ref]$cnt)
# also update the header note
$conv = $conv.Replace("// RAW extraction (may contain half-width katakana; zenkaku conversion applied separately)", "// Half-width katakana converted to full-width katakana (dakuten/handakuten combined)")
[IO.File]::WriteAllText($rc, $conv, (New-Object Text.UTF8Encoding($false)))
"converted halfwidth-kana chars=$cnt"
