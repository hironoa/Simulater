/*
 * native_ram_clear.c - exec = native のひな形: アセンブラで書かれた「RAM の範囲をクリアする」関数の、PC 用の代わり
 *
 * exec = native では、製品の C の変数はマイコンの RAM の番地ではなく、DLL の中の別の場所に置かれます。
 * そのため、番地を数字で渡して RAM をクリアする関数（アセンブラ）の代わりに「何もせず 0 を返す」関数が
 * 使われると、C の変数がクリアされず、WinAMS の結果と差が出ます。このファイルの関数を代わりに使うと、
 * sim が、ロードモジュールから読んだ「変数のマイコンでの番地」を見て、範囲の中の変数をクリアします。
 *
 * 使い方:
 *   1. このファイルを native_src のフォルダにコピーする（sim.cfg の native_src = で指定したフォルダ、またはファイル）。
 *   2. 関数名 ram_clear と引数を、製品のアセンブラの関数に合わせて書き換える
 *      （C から呼ばれているときの名前と引数。例: void ram_clear(u2 *first, u2 *last)）。
 *   3. 終わりの番地を含むかどうかを、製品の関数に合わせて選ぶ（下の 2 つの呼び出しのどちらか一方を使う）。
 *        sim_native_clear_range      [先頭, 終わり) をクリアする（終わりの番地は含まない）
 *        sim_native_clear_range_incl [先頭, 終わり] をクリアする（終わりの番地の 1 バイトまで含む）
 *   4. sim.cfg に native_layout = <製品のロードモジュール（.lmf か .abs）> を書く。これがないと、番地を直接書いた
 *      データ（0xF0000〜0xFFFFF）だけがクリアされ、C の変数はクリアされません（警告が 1 度出ます）。
 *      ロードモジュールは、製品のソースと同じビルドのものを使ってください（違うと番地がずれて、別の変数をクリアします）。
 *      製品だけをビルドしたものでも、テストドライバを含めてビルドしたものでもかまいません。WinAMS の結果と
 *      比べるなら、WinAMS に読み込ませているものを勧めます。
 *
 * クリアするもの・しないもの:
 *   - 製品のプロジェクト（mtpj）に登録されたソースの C の変数: クリアする（範囲に全部入っているものは全体、
 *     配列などが範囲の境界をまたぐときは、マイコンと PC で大きさが同じなら重なった部分だけ。大きさが違えば
 *     クリアせず、警告を 1 度出す）
 *   - 番地を直接書いたデータ（0xF0000〜0xFFFFF。製品のソースでも、テストドライバの #define の番地でも）: クリアする
 *   - native_src のソース（テストドライバ、スタブ、このファイル）の C の変数: クリアしない（範囲に入っていても残る）
 *   - const の変数: クリアしない
 *   範囲の外は触りません。
 *
 * 番地は整数にして渡します。ポインタは PC では 64 ビットなので、(unsigned long) を通して整数にします
 * （unsigned long は Windows では 32 ビットで、0xFD700 のような番地は入ります）。呼ぶ側が (u2 *)0xD700 のように
 * 16 ビットで書いていても、native_near（既定 on）で 0xFD700 になっています。sim_native_clear_range の宣言は、
 * sim が製品のソースの先頭に差し込むヘッダー（sim_native_pre.h）にあるので、このファイルに #include は要りません。
 */

/* 例: アセンブラで書かれた「RAM の範囲をクリアする」関数の、PC 用の代わり */
void ram_clear(unsigned short *first, unsigned short *last)
{
    /* 終わりの番地を含まない場合 */
    sim_native_clear_range((unsigned long long)(unsigned long)first, (unsigned long long)(unsigned long)last);
    /* 終わりの番地を含む場合は、上の行の代わりに:
    sim_native_clear_range_incl((unsigned long long)(unsigned long)first, (unsigned long long)(unsigned long)last);
    */
}
