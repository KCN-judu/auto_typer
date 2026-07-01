export function KeymapLegend() {
  return (
    <div className="kmLegend">
      <h4 className="kmLegendTitle">图例</h4>
      <div className="kmLegendItem">
        <span className="kmLegendDot normal" />
        <span>普通按键</span>
      </div>
      <div className="kmLegendItem">
        <span className="kmLegendDot highlight" />
        <span>搜索匹配 / 悬停</span>
      </div>
      <div className="kmLegendItem">
        <span className="kmLegendDot selected" />
        <span>已选中</span>
      </div>
      <div className="kmLegendItem">
        <span className="kmLegendDot head" />
        <span>当前打印头</span>
      </div>
    </div>
  );
}
