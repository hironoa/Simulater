import { useCompareView } from "../../hooks/useCompareView";
import { CompareStatus } from "./components/CompareStatus";
import React, { useState, useEffect, useRef, useMemo } from "react";
import { useWaveformStore } from "../../store/useWaveformStore";
import { useShallow } from "zustand/react/shallow";
import { cn } from "../../utils/helpers";
import { Settings, Plus, Layers, Rows, Link, ChevronDown, ChevronRight, Table2 } from "lucide-react";
import { DndContext, closestCenter, pointerWithin, KeyboardSensor, PointerSensor, useSensor, useSensors, DragEndEvent, DragOverEvent, useDroppable, DragStartEvent, DragOverlay, CollisionDetection } from "@dnd-kit/core";
import { arrayMove, SortableContext, sortableKeyboardCoordinates, verticalListSortingStrategy } from "@dnd-kit/sortable";
import { SignalItemOverlay, GroupItemOverlay, SortableGroupItem, GroupDroppableList, SortableSignalItem } from "./components/SignalList";
import { GroupSettings } from "./components/GroupSettings";
import { SignalTableDialog } from "./components/SignalTableDialog";
import { InactiveRulesEditor } from "./components/InactiveRulesEditor";

export function Sidebar() {
  const view = useCompareView();
  const {
      
    signalGroups,
    unassignedSignals,
    unassignedGroupConfig,
    activeSidebarTab,
    taskPeriod,
    signalSettings,
    setTaskPeriod,
    toggleAllSignals,
    addGroup,
    renameGroup,
    toggleGroupExpand,
    toggleGroupVisibility,
    setGroupLayout,
    toggleGroupSync,
    setGroups,
  
    } = useWaveformStore(useShallow((state) => ({
      signalGroups: state.signalGroups,
      unassignedSignals: state.unassignedSignals,
      unassignedGroupConfig: state.unassignedGroupConfig,
      activeSidebarTab: state.activeSidebarTab,
      taskPeriod: state.taskPeriod,
      signalSettings: state.signalSettings,
      setTaskPeriod: state.setTaskPeriod,
      toggleAllSignals: state.toggleAllSignals,
      addGroup: state.addGroup,
      renameGroup: state.renameGroup,
      toggleGroupExpand: state.toggleGroupExpand,
      toggleGroupVisibility: state.toggleGroupVisibility,
      setGroupLayout: state.setGroupLayout,
      toggleGroupSync: state.toggleGroupSync,
      setGroups: state.setGroups
    })));

  const [isModalOpen, setIsModalOpen] = useState(false);
  const [isSignalTableOpen, setIsSignalTableOpen] = useState(false);
  const [modalTitle, setModalTitle] = useState("");
  const [modalInput, setModalInput] = useState("");
  const [editingGroupId, setEditingGroupId] = useState<string | null>(null);
  const [activeDragId, setActiveDragId] = useState<string | null>(null);
  const [activeDragType, setActiveDragType] = useState<
    "Signal" | "Group" | null
  >(null);

  const sensors = useSensors(
    useSensor(PointerSensor, {
      activationConstraint: {
        distance: 5,
      },
    }),
    useSensor(KeyboardSensor, {
      coordinateGetter: sortableKeyboardCoordinates,
    }),
  );

  const handleDragStart = (event: DragStartEvent) => {
    setActiveDragId(String(event.active.id));
    setActiveDragType((event.active.data.current?.type as any) || null);
  };

  const handleDragCancel = () => {
    setActiveDragId(null);
    setActiveDragType(null);
  };

  const customCollisionDetection: CollisionDetection = (args) => {
    if (activeDragType === "Group") {
      return closestCenter({
        ...args,
        droppableContainers: args.droppableContainers.filter(
          (container) => container.data.current?.type === "Group",
        ),
      });
    }
    return pointerWithin(args);
  };

  const openAddGroupModal = () => {
    setEditingGroupId(null);
    setModalTitle("新規グループ追加");
    setModalInput(`グループ ${signalGroups.length + 1}`);
    setIsModalOpen(true);
  };

  const openRenameGroupModal = (id: string, currentName: string) => {
    setEditingGroupId(id);
    setModalTitle("グループ名変更");
    setModalInput(currentName);
    setIsModalOpen(true);
  };

  const confirmModal = () => {
    if (!modalInput.trim()) return;
    if (editingGroupId) {
      renameGroup(editingGroupId, modalInput.trim());
    } else {
      addGroup(modalInput.trim());
    }
    setIsModalOpen(false);
  };

  // For Dnd-kit: We need a flat list for simple usage, or multiple contexts.
  // Given the complexity of dragging between multiple lists, we'll keep it simple:
  // Reordering inside the list. For drag between groups, we will implement a custom handleDragEnd.
  const handleDragOver = (event: DragOverEvent) => {
    const { active, over } = event;
    if (!over) return;

    const activeType = active.data.current?.type;
    const overType = over.data.current?.type;

    const activeId = String(active.id);
    let overId = String(over.id);

    const findGroupAndIndex = (sigId: string) => {
      if (unassignedSignals.includes(sigId))
        return { group: "unassigned", index: unassignedSignals.indexOf(sigId) };
      for (let g of signalGroups) {
        const idx = g.signals.indexOf(sigId);
        if (idx !== -1) return { group: g.id, index: idx };
      }
      return null;
    };

    if (activeType === "Group") {
      return;
    }

    if (overType === "Container") {
      overId = over.data.current?.groupId;
    } else if (overType === "Group") {
      overId = String(over.id);
    }

    if (activeId === overId) return;

    const findContainer = (id: string) => {
      if (id === "unassigned") return "unassigned";
      if (signalGroups.some((g) => g.id === id)) return id;

      if (unassignedSignals.includes(id)) return "unassigned";
      for (const group of signalGroups) {
        if (group.signals.includes(id)) return group.id;
      }
      return null;
    };

    const activeContainer = findContainer(activeId);
    const overContainer = findContainer(overId);

    if (
      !activeContainer ||
      !overContainer ||
      activeContainer === overContainer
    ) {
      return;
    }

    const getContainerItems = (containerId: string) => {
      if (containerId === "unassigned") return [...unassignedSignals];
      return [
        ...(signalGroups.find((g) => g.id === containerId)?.signals || []),
      ];
    };

    const activeItems = getContainerItems(activeContainer);
    const overItems = getContainerItems(overContainer);

    const activeIndex = activeItems.indexOf(activeId);
    let overIndex;

    if (overId === overContainer) {
      overIndex = overItems.length;
    } else {
      overIndex = overItems.indexOf(overId);
      const isBelowOverItem =
        over &&
        active.rect.current.translated &&
        active.rect.current.translated.top > over.rect.top + over.rect.height;
      const modifier = isBelowOverItem ? 1 : 0;
      overIndex = overIndex >= 0 ? overIndex + modifier : overItems.length + 1;
    }

    const newActiveItems = [...activeItems];
    newActiveItems.splice(activeIndex, 1);

    const newOverItems = [...overItems];
    newOverItems.splice(overIndex, 0, activeId);

    if (activeContainer === "unassigned") {
      const newGroups = signalGroups.map((g) =>
        g.id === overContainer ? { ...g, signals: newOverItems } : g,
      );
      setGroups(newGroups, newActiveItems);
    } else if (overContainer === "unassigned") {
      const newGroups = signalGroups.map((g) =>
        g.id === activeContainer ? { ...g, signals: newActiveItems } : g,
      );
      setGroups(newGroups, newOverItems);
    } else {
      const newGroups = signalGroups.map((g) => {
        if (g.id === activeContainer) return { ...g, signals: newActiveItems };
        if (g.id === overContainer) return { ...g, signals: newOverItems };
        return g;
      });
      setGroups(newGroups, unassignedSignals);
    }
  };

  const handleDragEnd = (event: DragEndEvent) => {
    setActiveDragId(null);
    setActiveDragType(null);
    const { active, over } = event;
    if (!over) return;

    const activeType = active.data.current?.type;
    const overType = over.data.current?.type;

    const activeId = String(active.id);
    const overId = String(over.id);

    const findGroupAndIndex = (sigId: string) => {
      if (unassignedSignals.includes(sigId))
        return { group: "unassigned", index: unassignedSignals.indexOf(sigId) };
      for (let g of signalGroups) {
        const idx = g.signals.indexOf(sigId);
        if (idx !== -1) return { group: g.id, index: idx };
      }
      return null;
    };

    if (activeType === "Group") {
      let targetGroupId = overId;
      if (overType === "Signal") {
        const info = findGroupAndIndex(overId);
        targetGroupId = info ? info.group : overId;
      } else if (overType === "Container") {
        targetGroupId = over.data.current?.groupId || overId;
      }

      if (
        targetGroupId &&
        targetGroupId !== "unassigned" &&
        activeId !== targetGroupId
      ) {
        const activeIndex = signalGroups.findIndex((g) => g.id === activeId);
        const overIndex = signalGroups.findIndex((g) => g.id === targetGroupId);
        if (activeIndex !== -1 && overIndex !== -1) {
          const newGroups = arrayMove(signalGroups, activeIndex, overIndex);
          setGroups(newGroups, unassignedSignals);
        }
      }
      return;
    }

    if (
      activeId !== overId &&
      activeType === "Signal" &&
      overType === "Signal"
    ) {
      const activeInfo = findGroupAndIndex(activeId);
      const overInfo = findGroupAndIndex(overId);

      if (activeInfo && overInfo && activeInfo.group === overInfo.group) {
        if (activeInfo.group === "unassigned") {
          const newOrder = arrayMove(
            unassignedSignals,
            activeInfo.index,
            overInfo.index,
          );
          setGroups(signalGroups, newOrder);
        } else {
          const newGroups = signalGroups.map((g) => {
            if (g.id === activeInfo.group) {
              return {
                ...g,
                signals: arrayMove(g.signals, activeInfo.index, overInfo.index),
              };
            }
            return g;
          });
          setGroups(newGroups, unassignedSignals);
        }
      }
    }
  };

  if (activeSidebarTab !== "sig") {
    return null;
  }

  const allUnassignedChecked =
    unassignedSignals.length > 0 &&
    unassignedSignals.every((s) => signalSettings[s]?.checked);

  return (
    <div className="flex flex-col h-full bg-white dark:bg-slate-800 transition-colors">
      <div className="px-3.5 py-2.5 border-b border-gray-200 dark:border-slate-700 bg-gray-50 dark:bg-slate-900 font-bold dark:text-gray-200 transition-colors">
        信号設定 (ドラッグで並び替え)
        <div className="mt-1.5 flex items-center justify-between">
          <div className="flex gap-1">
            <button
              className="px-2 py-0.5 text-[11px] bg-blue-500 text-white rounded hover:bg-blue-600"
              onClick={() => toggleAllSignals(true)}
            >
              全選択
            </button>
            <button
              className="px-2 py-0.5 text-[11px] bg-gray-500 text-white rounded hover:bg-gray-600"
              onClick={() => toggleAllSignals(false)}
            >
              全解除
            </button>
            {Object.keys(signalSettings).length > 0 && (
              <button
                className="px-2 py-0.5 text-[11px] bg-white dark:bg-slate-700 text-blue-500 dark:text-blue-300 border border-blue-500 rounded hover:bg-blue-50 dark:hover:bg-slate-600 flex items-center gap-1"
                onClick={() => setIsSignalTableOpen(true)}
              >
                <Table2 size={12} />一覧編集
              </button>
            )}
          </div>
          <div className="text-[11px] text-gray-600 dark:text-gray-300 flex items-center gap-1 transition-colors">
            周期:
            <input
              type="number"
              value={taskPeriod}
              onChange={(e) => setTaskPeriod(parseFloat(e.target.value) || 5)}
              className="w-10 p-0.5 border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-800 rounded text-right text-[11px] outline-none transition-colors"
            />{" "}
            ms
          </div>
        </div>
        <InactiveRulesEditor />
      </div>

      <div className="flex-1 overflow-y-auto p-2.5">
        {Object.keys(view.settings).length === 0 ? (
          <p className="text-gray-400 text-sm text-center mt-5">
            CSVファイルを読み込んでください
          </p>
        ) : (
          <DndContext
            sensors={sensors}
            collisionDetection={customCollisionDetection}
            onDragStart={handleDragStart}
            onDragOver={handleDragOver}
            onDragEnd={handleDragEnd}
            onDragCancel={handleDragCancel}
          >
            <button
              onClick={openAddGroupModal}
              className="w-[calc(100%-20px)] m-2.5 p-1.5 bg-white dark:bg-slate-800 border border-dashed border-gray-400 dark:border-slate-500 rounded text-gray-600 dark:text-gray-300 text-xs hover:bg-blue-50 dark:hover:bg-slate-700 hover:border-blue-500 hover:text-blue-500 transition-colors"
            >
              ＋ 新規グループ追加
            </button>

            <SortableContext
              items={view.groups.map((g) => g.id)}
              strategy={verticalListSortingStrategy}
            >
              {view.groups.map((group) => (
                <SortableGroupItem
                  key={group.id}
                  group={group}
                  onRename={openRenameGroupModal}
                />
              ))}
            </SortableContext>

            {view.unassigned.length > 0 && (
              <div className="mb-2">
                <div
                  className="flex items-center bg-gray-200 dark:bg-slate-700 px-2.5 py-1.5 text-xs font-bold border-b border-gray-300 dark:border-slate-600 dark:text-gray-200 cursor-pointer hover:bg-gray-300 dark:hover:bg-slate-600 select-none transition-colors"
                  onClick={() =>
                    toggleGroupVisibility("unassigned", !allUnassignedChecked)
                  }
                >
                  <span
                    className="mr-1 w-3 text-center text-[10px]"
                    onClick={(e) => {
                      e.stopPropagation();
                      toggleGroupExpand("unassigned");
                    }}
                  >
                    {unassignedGroupConfig.expanded ? (
                      <ChevronDown size={12} />
                    ) : (
                      <ChevronRight size={12} />
                    )}
                  </span>
                  <input
                    type="checkbox"
                    checked={allUnassignedChecked}
                    onChange={(e) => {
                      toggleGroupVisibility(
                        "unassigned",
                        !allUnassignedChecked,
                      );
                    }}
                    onClick={(e) => e.stopPropagation()}
                    onPointerDown={(e) => e.stopPropagation()}
                    className="mr-1"
                  />
                  <span className="flex-1 ml-1 truncate text-gray-500 dark:text-gray-400 text-left">
                    未分類
                  </span>
                  <div
                    className="flex gap-1.5 items-center"
                    onClick={(e) => e.stopPropagation()}
                    onPointerDown={(e) => e.stopPropagation()}
                  >
                    <div className="flex shadow-sm rounded">
                      <button
                        className={cn(
                          "p-1 text-[10px] flex items-center justify-center border rounded-l transition-colors",
                          unassignedGroupConfig.layout === "overlap"
                            ? "bg-blue-500 text-white border-blue-500 z-10"
                            : "bg-white dark:bg-slate-700 text-gray-600 dark:text-gray-300 border-gray-300 dark:border-slate-600 hover:bg-gray-50 dark:hover:bg-slate-600 z-0",
                        )}
                        onClick={() => setGroupLayout("unassigned", "overlap")}
                        title="重ね表示"
                      >
                        <Layers size={14} />
                      </button>
                      <button
                        className={cn(
                          "p-1 text-[10px] flex items-center justify-center border -ml-px rounded-r transition-colors",
                          unassignedGroupConfig.layout === "lane"
                            ? "bg-blue-500 text-white border-blue-500 z-10"
                            : "bg-white dark:bg-slate-700 text-gray-600 dark:text-gray-300 border-gray-300 dark:border-slate-600 hover:bg-gray-50 dark:hover:bg-slate-600 z-0",
                        )}
                        onClick={() => setGroupLayout("unassigned", "lane")}
                        title="レーン表示"
                      >
                        <Rows size={14} />
                      </button>
                    </div>
                    <button
                      className={cn(
                        "p-1 bg-white dark:bg-slate-700 border border-gray-300 dark:border-slate-600 rounded flex items-center justify-center text-gray-600 dark:text-gray-300 shadow-sm transition-colors hover:bg-gray-50 dark:hover:bg-slate-600",
                        unassignedGroupConfig.syncRange &&
                          "bg-blue-500 text-white border-blue-500 hover:bg-blue-600",
                      )}
                      onClick={() => toggleGroupSync("unassigned")}
                      title="同一レンジ（Y軸同期）"
                    >
                      <Link size={14} />
                    </button>
                  </div>
                </div>
                {unassignedGroupConfig.expanded && (
                  <GroupDroppableList
                    id="unassigned"
                    signals={view.unassigned}
                  />
                )}
              </div>
            )}

            <DragOverlay>
              {activeDragId && activeDragType === "Signal" ? (
                <SignalItemOverlay sigName={activeDragId} />
              ) : null}
              {activeDragId && activeDragType === "Group" ? (
                <GroupItemOverlay id={activeDragId} />
              ) : null}
            </DragOverlay>
          </DndContext>
        )}
      </div>

      <CompareStatus />

      {isSignalTableOpen && <SignalTableDialog onClose={() => setIsSignalTableOpen(false)} />}

      {isModalOpen && (
        <div className="fixed inset-0 bg-black/50 z-[9999] flex justify-center items-center">
          <div className="bg-white dark:bg-slate-800 dark:text-gray-200 p-5 rounded max-w-sm w-full shadow-lg transition-colors">
            <h3 className="mt-0 mb-4 text-sm font-bold">{modalTitle}</h3>
            <input
              type="text"
              value={modalInput}
              onChange={(e) => setModalInput(e.target.value)}
              onKeyDown={(e) => e.key === "Enter" && confirmModal()}
              className="w-full p-2 mb-4 border border-gray-300 dark:border-slate-600 bg-white dark:bg-slate-700 rounded focus:border-blue-500 outline-none transition-colors"
              autoFocus
            />
            <div className="text-right">
              <button
                onClick={() => setIsModalOpen(false)}
                className="px-3 py-1.5 mr-2 text-blue-500 bg-transparent border border-blue-500 rounded hover:bg-blue-50"
              >
                キャンセル
              </button>
              <button
                onClick={confirmModal}
                className="px-3 py-1.5 text-white bg-blue-500 rounded hover:bg-blue-600"
              >
                確定
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
