//-----------------------------------
// Copyright Pierric Gimmig 2013-2017
//-----------------------------------
#pragma once

#include "DataView.h"
#include "Message.h"
#include "Threading.h"

struct CallStack;

//-----------------------------------------------------------------------------
class LogDataView : public DataView {
 public:
  LogDataView();
  const std::vector<std::string>& GetColumnHeaders() override;
  const std::vector<float>& GetColumnHeadersRatios() override;
  std::vector<std::string> GetContextMenu(
      int a_ClickedIndex, const std::vector<int>& a_SelectedIndices) override;
  std::string GetValue(int a_Row, int a_Column) override;
  std::string GetToolTip(int a_Row, int a_Column) override;
  bool ScrollToBottom() override;
  bool SkipTimer() override;

  void OnDataChanged() override;
  void OnFilter(const std::string& a_Filter) override;
  void OnContextMenu(const std::string& a_Action, int a_MenuIndex,
                     const std::vector<int>& a_ItemIndices) override;

  void Add(const OrbitLogEntry& a_Msg);
  const OrbitLogEntry& GetEntry(unsigned int a_Row) const;
  void OnReceiveMessage(const Message& a_Msg);

  enum OdvColumn { LDV_Message, LDV_Time, LDV_ThreadId, LDV_NumColumns };

 protected:
  std::vector<OrbitLogEntry> m_Entries;
  Mutex m_Mutex;
  std::shared_ptr<CallStack> m_SelectedCallstack;

  static void InitColumnsIfNeeded();
  static std::vector<std::string> s_Headers;
  static std::vector<float> s_HeaderRatios;
  static std::vector<SortingOrder> s_InitialOrders;
};
