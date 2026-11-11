import { domToPng } from 'modern-screenshot';
import * as echarts from 'echarts';
import { renderOverlayLabels, buildCursorLabelGroups } from '../components/Chart';
import { useWaveformStore } from '../store/useWaveformStore';

/**
 * グラフおよび計測表を画像化し、クリップボードにコピーする。
 */
export const copyChartAndTableToClipboard = async (
  hasMarkers: boolean,
  theme: 'light' | 'dark',
  layout: 'only-chart' | 'vertical-normal' | 'vertical-transposed' | 'horizontal-normal' = 'vertical-normal'
): Promise<void> => {
  const chartContainer = document.getElementById('chart-container');
  if (!chartContainer) {
    throw new Error("チャート要素が見つかりません。");
  }

  const chartInstance = echarts.getInstanceByDom(chartContainer);
  if (!chartInstance) {
    throw new Error("EChartsインスタンスが見つかりません。");
  }

  const generateBlob = async (): Promise<Blob> => {
    let tableImg: HTMLImageElement | null = null;
    let tableOriginalWidth = 0;

    // マーカーがある場合は計測表も抽出
    if (hasMarkers && layout !== 'only-chart') {
      const isTransposed = layout === 'vertical-transposed' || layout === 'horizontal-normal';
      const isHorizontalLayout = layout === 'horizontal-normal';

      const tableId = isTransposed ? 'export-measure-table-transposed' : 'export-measure-table-normal';
      const tableContainer = document.getElementById(tableId);
      if (tableContainer) {
        // 横配列（上下レイアウト）の場合、画面と同じように改行させるために事前にラッパーの幅をチャートに合わせる
        if (!isHorizontalLayout && !isTransposed) {
          tableContainer.style.width = `${chartContainer.offsetWidth}px`;
        }

        const clone = tableContainer.cloneNode(true) as HTMLElement;
        
        // オフスクリーンに展開
        if (isHorizontalLayout) {
          // 左右レイアウトでは max-content にして余分な余白をなくす
          clone.style.width = 'max-content';
        } else {
          // 上下レイアウトでは、長すぎて改行が解けるのを防ぐために実際のテーブルの幅に固定する
          const originalTableElement = tableContainer.querySelector('table');
          const targetWidth = originalTableElement ? originalTableElement.offsetWidth : tableContainer.scrollWidth;
          clone.style.width = `${targetWidth}px`;
        }
        clone.classList.remove('w-full');

        // テーブル自体の 100% 指定などを削る (余計な余白の原因になるため)
        const tableElement = clone.querySelector('table');
        if (tableElement) {
          tableElement.classList.remove('w-full', 'min-w-full');
        }

        clone.style.height = 'max-content';
        clone.style.position = 'fixed';
        clone.style.left = '-9999px';
        clone.style.top = '-9999px';
        clone.style.overflow = 'visible';

        // 不要なUI（ツールバー）を削除
        const tools = clone.querySelector('.flex.flex-wrap.items-center');
        if (tools) tools.remove();

        // スクロールエリアを見えるように
        const scrollArea = clone.querySelector('.flex-1.overflow-auto');
        if (scrollArea) {
          (scrollArea as HTMLElement).style.overflow = 'visible';
        }

        // キャプチャの際にsticky要素がズレるのを防ぐために、stickyを無効化する
        const stickyElements = clone.querySelectorAll('.sticky');
        stickyElements.forEach(el => {
          (el as HTMLElement).style.position = 'static';
        });

        // 指定された無視要素（アクション、ホバー列等）を削除
        const actionElements = clone.querySelectorAll('.export-ignore-action');
        actionElements.forEach(el => el.remove());
        const hoverElements = clone.querySelectorAll('.export-ignore-hover');
        hoverElements.forEach(el => el.remove());
        const buttons = clone.querySelectorAll('button');
        buttons.forEach(btn => btn.remove());

        document.body.appendChild(clone);

        try {
          tableOriginalWidth = clone.offsetWidth;
          const exportWidth = clone.offsetWidth; // No 1200px limit
          const dataUrl = await domToPng(clone, {
            scale: 1,
            backgroundColor: theme === 'dark' ? '#1e293b' : '#ffffff', // slate-800 corresponds to #1e293b
            width: exportWidth,
          });
          tableImg = await loadImage(dataUrl);
        } finally {
          document.body.removeChild(clone);
        }
      }
    }

    // EChartsから画像抽出
    let chartDataUrl = '';
    const tempContainerWrapper = document.createElement('div');
    const tempChartContainer = document.createElement('div');
    const tempOverlayContainer = document.createElement('div');
    
    const isHorizontalLayout = layout === 'horizontal-normal';

    // 上下レイアウトで表が大きい場合は表の幅に合わせる、それ以外は元のチャート幅
    const targetChartWidth = (!isHorizontalLayout && tableImg && tableOriginalWidth > chartContainer.offsetWidth)
      ? tableOriginalWidth
      : chartContainer.offsetWidth;

    tempContainerWrapper.style.width = targetChartWidth + 'px';
    tempContainerWrapper.style.height = chartContainer.offsetHeight + 'px';
    tempContainerWrapper.style.position = 'absolute';
    tempContainerWrapper.style.left = '-9999px';
    tempContainerWrapper.style.top = '-9999px';
    tempContainerWrapper.style.backgroundColor = theme === 'dark' ? '#0f172a' : '#ffffff';

    tempChartContainer.style.width = '100%';
    tempChartContainer.style.height = '100%';
    tempChartContainer.style.position = 'absolute';
    tempChartContainer.style.left = '0';
    tempChartContainer.style.top = '0';

    tempOverlayContainer.style.width = '100%';
    tempOverlayContainer.style.height = '100%';
    tempOverlayContainer.style.position = 'absolute';
    tempOverlayContainer.style.left = '0';
    tempOverlayContainer.style.top = '0';
    tempOverlayContainer.style.pointerEvents = 'none';

    tempContainerWrapper.appendChild(tempChartContainer);
    tempContainerWrapper.appendChild(tempOverlayContainer);
    document.body.appendChild(tempContainerWrapper);

    try {
      const tempChart = echarts.init(tempChartContainer, null, { renderer: 'canvas' });
      const option = chartInstance.getOption() as any;
      
      // アニメーションを無効化して即時反映させる
      if (option) {
        option.animation = false;
        // マウス追従カーソルを画像から確実に消す
        if (option.tooltip) option.tooltip.show = false;
        if (option.xAxis && Array.isArray(option.xAxis)) {
          option.xAxis.forEach((x: any) => {
            if (x.axisPointer) x.axisPointer.show = false;
          });
        }
      }
      
      // 新しいインスタンスに現在の状態（オプション）を流し込む
      tempChart.setOption(option, true);
      
      // レンダリング完了を少し待機
      await new Promise(resolve => setTimeout(resolve, 50));

      // グラフ描画後に、カーソルのラベルをオーバーレイとして描画する
      if (option && option.series) {
        const labelGroups = buildCursorLabelGroups(tempChart, theme, true);
        const { showCursorLabels, cursors, showMarkers } = useWaveformStore.getState();
        renderOverlayLabels(tempOverlayContainer, labelGroups, false, showCursorLabels !== "off", cursors, tempChart, theme, false, showMarkers);
      }

      // tempContainerWrapper全体をキャプチャする（domToPngを利用）
      chartDataUrl = await domToPng(tempContainerWrapper, {
        scale: 1,
        backgroundColor: theme === 'dark' ? '#0f172a' : '#ffffff',
        width: targetChartWidth,
        height: chartContainer.offsetHeight
      });

      tempChart.dispose();
    } finally {
      document.body.removeChild(tempContainerWrapper);
    }
    const chartImg = await loadImage(chartDataUrl);

    // Canvas合成
    const mergedCanvas = document.createElement('canvas');
    const ctx = mergedCanvas.getContext('2d');
    if (!ctx) throw new Error("Canvas contextを取得できませんでした。");

    if (tableImg) {
      if (layout === 'horizontal-normal') {
        // 左右レイアウト (表が左、波形が右)
        const chartW = chartImg.width;
        const chartH = chartImg.height;
        const tableW = tableImg.width;
        const tableH = tableImg.height;

        mergedCanvas.width = chartW + tableW;
        mergedCanvas.height = Math.max(chartH, tableH);

        ctx.fillStyle = theme === 'dark' ? '#0f172a' : '#ffffff';
        ctx.fillRect(0, 0, mergedCanvas.width, mergedCanvas.height);

        // 表を左、波形を右
        ctx.drawImage(tableImg, 0, 0, tableW, tableH);
        ctx.drawImage(chartImg, tableW, 0, chartW, chartH);
      } else {
        // 上下レイアウト (波形が上、表が下)
        const chartW = chartImg.width;
        const chartH = chartImg.height;
        const tableW = tableImg.width;
        const tableH = tableImg.height;

        mergedCanvas.width = Math.max(chartW, tableW);
        mergedCanvas.height = chartH + tableH;

        ctx.fillStyle = theme === 'dark' ? '#0f172a' : '#ffffff';
        ctx.fillRect(0, 0, mergedCanvas.width, mergedCanvas.height);

        ctx.drawImage(chartImg, 0, 0, chartW, chartH);
        ctx.drawImage(tableImg, 0, chartH, tableW, tableH);
      }
    } else {
      mergedCanvas.width = chartImg.width;
      mergedCanvas.height = chartImg.height;
      ctx.fillStyle = theme === 'dark' ? '#0f172a' : '#ffffff';
      ctx.fillRect(0, 0, mergedCanvas.width, mergedCanvas.height);
      ctx.drawImage(chartImg, 0, 0, chartImg.width, chartImg.height);
    }

    // 罫線を描画する (余白なし)
    const strokeColor = theme === 'dark' ? '#475569' : '#cbd5e1'; // slate-600 : slate-300
    ctx.lineWidth = 2;
    ctx.strokeStyle = strokeColor;

    // 画像全体の外周に罫線を描画
    ctx.strokeRect(1, 1, mergedCanvas.width - 2, mergedCanvas.height - 2);

      // 波形と計測表の間に境界線を描画
      if (tableImg) {
        ctx.beginPath();
        if (layout === 'horizontal-normal') {
          const tableW = tableImg.width;
          ctx.moveTo(tableW, 0);
          ctx.lineTo(tableW, mergedCanvas.height);
        } else {
          ctx.moveTo(0, chartImg.height);
          ctx.lineTo(mergedCanvas.width, chartImg.height);
        }
        ctx.stroke();
      }

      return new Promise<Blob>((resolve, reject) => {
        mergedCanvas.toBlob((blob) => {
          if (blob) resolve(blob);
          else reject(new Error("Blobの生成に失敗しました。"));
        }, 'image/png');
      });
  };

  try {
    // Safari等ではPromiseを直接ClipboardItemに渡す必要がある場合がある
    // Note: TypeScriptの型エラーを避けるため any へキャスト
    const promise = generateBlob();
    const item = new window.ClipboardItem({ 'image/png': promise } as any);
    await navigator.clipboard.write([item]);
  } catch (error) {
    console.warn("Clipboard with Promise failed, fallback to sequential execution", error);
    // Firefoxや古い実装では、直接Blobを渡す
    const blob = await generateBlob();
    const item = new window.ClipboardItem({ 'image/png': blob });
    await navigator.clipboard.write([item]);
  }
};

const loadImage = (src: string): Promise<HTMLImageElement> => {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.crossOrigin = "anonymous";
    img.onload = () => resolve(img);
    img.onerror = reject;
    img.src = src;
  });
};
