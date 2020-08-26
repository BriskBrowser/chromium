/*
 * Copyright (C) 2012 Apple Inc. All rights reserved.
 * Copyright (C) 2013 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTOR_PAGE_STREAM_AGENT_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTOR_PAGE_STREAM_AGENT_H_

#include "base/macros.h"
#include "base/memory/scoped_refptr.h"
#include "ui/compositor/compositor_lock.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/inspector/inspector_base_agent.h"
#include "third_party/blink/renderer/core/inspector/protocol/PageStream.h"
#include "third_party/blink/renderer/platform/timer.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace cc {
class Layer;
}

namespace blink {

class InspectedFrames;
class PictureSnapshot;

typedef char TileQuality;
constexpr TileQuality kTileQualityDirty = 0;
constexpr TileQuality kTileQualityLowRes = 1;
constexpr TileQuality kTileQualityHighRes = 2;


class CORE_EXPORT InspectorPageStreamAgent final
    : public InspectorBaseAgent<protocol::PageStream::Metainfo> {
 public:
  class ClientSideLayer;
  friend class ClientSideLayer;

  InspectorPageStreamAgent(InspectedFrames*);
  ~InspectorPageStreamAgent() override;
  void Trace(blink::Visitor*) override;

  void Restore() override;

  // Called from InspectorInstrumentation
  void LayerTreeDidChange();
  void LayerTreePainted();

  // Called from the front-end.
  protocol::Response enable(
  	protocol::Maybe<int> target_bandwidth,
  	protocol::Maybe<int> fps,
  	protocol::Maybe<bool> send_click_targets,
  	protocol::Maybe<bool> auto_open_click_targets) override;
  
  protocol::Response disable() override;

  void flush(std::unique_ptr<FlushCallback>) override;
  
  protocol::Response setScroll(int backend_node_id, int x, int y)
      override;

  protocol::Response clickNode(int backend_node_id) override;
protected:
  float GetDPR();

 private:
  void LayerTreeDidChangeInternal(bool);
  
  const cc::Layer* RootLayer();
  void LayerRefreshComplete(bool all_done);

  void updateClickTargets();

  Member<InspectedFrames> inspected_frames_;

  bool pending_click_target_update_;
  int pending_frame_refreshs_;

  bool frame_is_queued_;
  bool layer_refresh_missed_deadline_;
  TileQuality layer_refresh_quality_;

  using LayerMap = HashMap<cc::Layer*, scoped_refptr<ClientSideLayer>>;
  LayerMap layers_;

  std::string prop_trees_;

  // Config from client
  InspectorAgentState::Integer target_bandwidth_;
  InspectorAgentState::Integer fps_;
  InspectorAgentState::Boolean send_click_targets_;
  InspectorAgentState::Boolean auto_open_click_targets_;

  InspectorAgentState::Boolean enabled_;

  std::vector<std::unique_ptr<FlushCallback>> flush_callbacks_;

  DISALLOW_COPY_AND_ASSIGN(InspectorPageStreamAgent);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTOR_PAGE_STREAM_AGENT_H_
