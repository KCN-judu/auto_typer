import type { KeymapVisualNode } from "../../domain/keymapLayout";

interface KeymapInspectorProps {
  node: KeymapVisualNode | null;
}

export function KeymapInspector({ node }: KeymapInspectorProps) {
  return (
    <div className="kmInspector">
      <h3 className="kmInspectorTitle">检查器</h3>
      {node ? (
        <>
          <div className="kmInspectorChar">{node.label}</div>
          <div className="kmInspectorGrid">
            <div className="kmInspectorRow">
              <span className="kmInspectorLabel">字符</span>
              <span className="kmInspectorValue">{node.label}</span>
            </div>
            <div className="kmInspectorRow">
              <span className="kmInspectorLabel">X (mm)</span>
              <span className="kmInspectorValue">{node.xMm.toFixed(2)}</span>
            </div>
            <div className="kmInspectorRow">
              <span className="kmInspectorLabel">Y (mm)</span>
              <span className="kmInspectorValue">{node.yMm.toFixed(2)}</span>
            </div>
            <div className="kmInspectorRow">
              <span className="kmInspectorLabel">序号</span>
              <span className="kmInspectorValue">{node.index}</span>
            </div>
            <div className="kmInspectorRow">
              <span className="kmInspectorLabel">Key</span>
              <span className="kmInspectorValue">{JSON.stringify(node.key)}</span>
            </div>
          </div>
        </>
      ) : (
        <p className="kmInspectorEmpty">悬停或点击一个按键节点查看详情</p>
      )}
    </div>
  );
}
