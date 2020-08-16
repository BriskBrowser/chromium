/*
 * Copyright (C) 2012 Apple Inc. All rights reserved.
 * Copyright (C) 2013 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "third_party/blink/renderer/core/inspector/inspector_page_stream_agent.h"

#include <memory>

#include "base/debug/stack_trace.h"
#include "base/stl_util.h"
#include "base/json/json_writer.h"
#include "cc/base/region.h"
#include "cc/layers/picture_layer.h"
#include "cc/trees/transform_node.h"
#include "cc/trees/clip_node.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/dom_node_ids.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/frame/root_frame_viewport.h"
#include "third_party/blink/renderer/core/frame/visual_viewport.h"
#include "third_party/blink/renderer/core/inspector/identifiers_factory.h"
#include "third_party/blink/renderer/core/inspector/inspected_frames.h"
#include "third_party/blink/renderer/core/layout/layout_embedded_content.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/loader/document_loader.h"
#include "third_party/blink/renderer/core/paint/paint_layer.h"
#include "third_party/blink/renderer/core/page/chrome_client.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/platform/geometry/int_rect.h"
#include "third_party/blink/renderer/platform/graphics/compositing_reasons.h"
#include "third_party/blink/renderer/platform/graphics/compositor_element_id.h"
#include "third_party/blink/renderer/platform/graphics/graphics_layer.h"
#include "third_party/blink/renderer/platform/graphics/picture_snapshot.h"
#include "third_party/blink/renderer/platform/scheduler/public/post_cross_thread_task.h"
#include "third_party/blink/renderer/platform/scheduler/public/thread.h"
#include "third_party/blink/renderer/platform/scheduler/public/worker_pool.h"
#include "third_party/blink/renderer/platform/transforms/transformation_matrix.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/text/base64.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"
#include "third_party/skia/include/core/SkPicture.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/core/SkRefCnt.h"
#include "ui/gfx/geometry/rect.h"

namespace blink {

using protocol::Array;
using protocol::Maybe;
using protocol::Response;

typedef blink::protocol::PageStream::Backend::FlushCallback FlushCallback;

static std::unique_ptr<protocol::DOM::Rect> BuildObjectForRect(
    const gfx::Rect& rect) {
  return protocol::DOM::Rect::create()
      .setX(rect.x())
      .setY(rect.y())
      .setHeight(rect.height())
      .setWidth(rect.width())
      .build();
}

std::string GetPropertyTreesJSON(cc::PropertyTrees* p) {
  base::trace_event::TracedValueJSON value;
  value.BeginDictionary("transform_tree");
  p->transform_tree.AsValueInto(&value);
  value.EndDictionary();

  value.BeginDictionary("effect_tree");
  p->effect_tree.AsValueInto(&value);
  value.EndDictionary();

  value.BeginDictionary("clip_tree");
  p->clip_tree.AsValueInto(&value);
  value.EndDictionary();

  value.BeginDictionary("scroll_tree");
  p->scroll_tree.AsValueInto(&value);
  value.EndDictionary();

  return value.ToJSON();
}

/* Keeps track of which areas of a layer are dirty */
// TODO:  This could be made much much more accurate...
class RegionStateTracker {
public:
  RegionStateTracker() {}

private:
  gfx::Rect internal_;


public:
  void  AddRect(gfx::Rect a) {
    internal_.Union(a);
  }
  std::unique_ptr<std::vector<gfx::Rect>> FetchAndResetDirtyRegions() {
    auto ret = std::make_unique<std::vector<gfx::Rect>>();
    ret->emplace_back(internal_);
    internal_ = gfx::Rect();
    return ret;
  }
};

InspectorPageStreamAgent::InspectorPageStreamAgent(
    InspectedFrames* inspected_frames)
    : inspected_frames_(inspected_frames),
    pending_click_target_update_(false), 
    pending_frame_refreshs_(0), 
    target_bandwidth_(&agent_state_, /*default_value=*/-1),
    fps_(&agent_state_, /*default_value=*/-1),
    send_click_targets_(&agent_state_, /*default_value=*/true),
    auto_open_click_targets_(&agent_state_, /*default_value=*/false),
    enabled_(&agent_state_, /*default_value=*/false)
     { }

InspectorPageStreamAgent::~InspectorPageStreamAgent() = default;

void InspectorPageStreamAgent::Trace(blink::Visitor* visitor) {
  visitor->Trace(inspected_frames_);
  InspectorBaseAgent::Trace(visitor);
}

void InspectorPageStreamAgent::Restore() {
  if (enabled_.Get()) {
    enable({}, {}, {}, {});
    GetFrontend()->debugInfo("reset");
  }
}

Response InspectorPageStreamAgent::enable(Maybe<int> target_bandwidth, Maybe<int> fps, Maybe<bool> send_click_targets, Maybe<bool> auto_open_click_targets) {
  instrumenting_agents_->AddInspectorPageStreamAgent(this);
  Document* document = inspected_frames_->Root()->GetDocument();
  if (!document)
    return Response::Error("The root frame doesn't have document");

  if (RootLayer()) for (auto* layer : *(RootLayer()->layer_tree_host()))
    layer->SetNeedsDisplay();

  enabled_.Set(true);

  if (target_bandwidth.isJust()) target_bandwidth_.Set(target_bandwidth.fromJust());
  if (fps.isJust()) fps_.Set(fps.fromJust());
  if (send_click_targets.isJust()) send_click_targets_.Set(send_click_targets.fromJust());
  if (auto_open_click_targets.isJust()) auto_open_click_targets_.Set(auto_open_click_targets.fromJust());

  return Response::OK();
}


gfx::Rect GetVisibleRect(cc::Layer* l) {
  // Visible region in screen space
  cc::LayerTreeHost* lth = l->layer_tree_host();
  if (!lth) return gfx::Rect();

  gfx::RectF visible_region = lth->property_trees()->clip_tree.Node(l->clip_tree_index())->cached_accumulated_rect_in_screen_space;

  // Translate screen to layer coordinates.
  const cc::EffectNode* root_effect_node =
      lth->property_trees()->effect_tree.Node(cc::EffectTree::kContentsRootNodeId);

  gfx::Transform target_to_local;
  lth->property_trees()->GetFromTarget(
      l->transform_tree_index(), root_effect_node->id, &target_to_local);

  gfx::RectF visible_region_in_layer = cc::MathUtil::ProjectClippedRect(target_to_local, visible_region);
  visible_region_in_layer.Offset(-l->offset_to_transform_parent());

  return gfx::ToEnclosingRect(visible_region_in_layer);

}

String RenderPicture(sk_sp<SkPicture> input, const gfx::Rect& clip_rect,
                                        double scale, int quality) {

  const SkIRect clip = SkIRect::MakeXYWH(clip_rect.x(), clip_rect.y(),
                                     clip_rect.width(),
                                     clip_rect.height());
  
  int width = ceil(scale * clip.width());
  int height = ceil(scale * clip.height());

  sk_sp<SkSurface> surface = SkSurface::MakeRasterN32Premul(width, height);
  SkCanvas* canvas = surface->getCanvas();

  canvas->scale(scale, scale);
  canvas->translate(-clip_rect.x(), -clip_rect.y());
  
  input->playback(canvas, nullptr);

  sk_sp<SkImage> img(surface->makeImageSnapshot());

  //DCHECK(img) << "No image returned";
  if (!img) return "";

  sk_sp<SkData> webp(img->encodeToData(SkEncodedImageFormat::kWEBP, quality));
  //DCHECK(webp) << "No webp data";
  if (!webp) return "";

  return "data:image/webp;base64," + Base64Encode(base::span<const uint8_t>(webp->bytes(), webp->size()));
}

void RenderPictureAndPostResult(scoped_refptr<base::SingleThreadTaskRunner> task_runner,
                                sk_sp<SkPicture> input, std::unique_ptr<gfx::Rect> clip_rect,
                                double scale, int quality,
                                WTF::CrossThreadOnceFunction<void(std::unique_ptr<protocol::PageStream::BufferUpdate>)> result_callback) {
  String imagedata = RenderPicture(input, *clip_rect, scale, quality);

  auto buf_msg = protocol::PageStream::BufferUpdate::create()
    .setImage(imagedata.IsolatedCopy())
    .setClip(BuildObjectForRect(*clip_rect))
    .build();

  PostCrossThreadTask(*task_runner, FROM_HERE,
                      CrossThreadBindOnce(std::move(result_callback), std::move(buf_msg)));
}


/* Responsible for syncing state of a cc:Layer to the client */
class InspectorPageStreamAgent::ClientSideLayer : public ThreadSafeRefCounted<ClientSideLayer> {
public:

  ClientSideLayer(cc::Layer* l, InspectorPageStreamAgent* ins) : 
    layer_(base::WrapRefCounted<cc::Layer>(l)), 
    layer_id_(l->id()),
    layer_as_string_(),
    z_index_(0),
    z_index_changed_(false),
    outstanding_images_(0),
    callback_(),
    dirty_(),
    deleted_(false),
    click_targets_(),
    ins_(ins) {}

  void Delete() {
    ins_->GetFrontend()->streamLayerInfo(protocol::PageStream::LayerUpdate::create()
        .setLayerId(layer_id_)
        .setLayerDeleted(true)
        .build());
    deleted_ = true;
    ins_ = nullptr;
  }

  void MakeDirty() {
    dirty_.AddRect(layer_->update_rect());
  }

  void Refresh(int quality, base::OnceCallback<void()> callback) {
    DCHECK(callback_.is_null());
    callback_ = std::move(callback);

    auto msg = protocol::PageStream::LayerUpdate::create()
                  .setLayerId(layer_id_)
                  .build();

    std::string new_layer_as_string(layer_->ToString());
    if (layer_as_string_ != new_layer_as_string) {
      layer_as_string_ = std::move(new_layer_as_string);
      msg->setLayerInfo(layer_as_string_.c_str());
    }

    auto regions = dirty_.FetchAndResetDirtyRegions();

    
    sk_sp<SkPicture> pic;
    gfx::Rect bounds(layer_->bounds());

    gfx::Rect visible_region = GetVisibleRect(layer_.get());

#define CELL_SIZE 256
    for (auto rect : *regions) {
      for (int cell_x=0; cell_x<=(bounds.width()-1)/CELL_SIZE; cell_x++) {
        for (int cell_y=0; cell_y<=(bounds.height()-1)/CELL_SIZE; cell_y++) {

          
          auto clip_rect = std::make_unique<gfx::Rect>(rect);
          gfx::Rect tile_rect(cell_x*CELL_SIZE, cell_y*CELL_SIZE, CELL_SIZE, CELL_SIZE);

          clip_rect->Intersect(tile_rect);

          
          float scale = 0.25;
          bool highres = visible_region.Intersects(tile_rect);

          auto cell = std::make_pair(cell_x, cell_y);

          if (highres) scale = 1.0;

          if (highres && tile_is_low_res_.count(cell)) {
            // was low res, now high res
            clip_rect = std::make_unique<gfx::Rect>(tile_rect);
            tile_is_low_res_.erase(cell);
          } else if (!highres && !tile_is_low_res_.count(cell)) {
            if (clip_rect->IsEmpty()) continue;
                    
            tile_is_low_res_.insert(cell);
          }

          if (clip_rect->IsEmpty()) continue;

          if (!pic) pic = layer_->GetPicture();
          if (!pic) continue;

          scale *= ins_->GetDPR();

          //scale = scale * 0.125;
          quality = 10;

          outstanding_images_++;

          worker_pool::PostTask(
              FROM_HERE,
              CrossThreadBindOnce(
                  RenderPictureAndPostResult, Thread::Current()->GetTaskRunner(),
                  pic, std::move(clip_rect), scale, quality,
                  CrossThreadBindOnce(&InspectorPageStreamAgent::ClientSideLayer::commitImage,
                                      WrapRefCounted(this))));
        }
      }
    }

    if (z_index_changed_) {
      msg->setZIndex(z_index_);
      z_index_changed_ = false;
    }

    if (msg->hasZIndex() || msg->hasTargets() || msg->hasBufferUpdates() || msg->hasLayerInfo())
      ins_->GetFrontend()->streamLayerInfo(std::move(msg));

    if (!outstanding_images_) std::move(callback_).Run();
  }

  void zIndex(int z) {
    if (z_index_==z) return;
    z_index_changed_ = true;
    z_index_ = z;
  }

  void updateClickTargets(std::map<int, std::unique_ptr<protocol::PageStream::ClickTarget>>& click_targets) {
    std::unique_ptr<protocol::Array<protocol::PageStream::ClickTarget>> targets = std::make_unique<protocol::Array<protocol::PageStream::ClickTarget>>();
    // Notify of deleted targets
    for (auto &m : click_targets_) {
      if (!click_targets.count(m.first))
        targets->emplace_back(
            protocol::PageStream::ClickTarget::create()
                .setBackendNodeId(m.first)
                .setTargetDeleted(true)
                .build());
    }
    // New or changed
    for (auto &m : click_targets) {
      protocol::Array<protocol::Array<double>> empty = {};

      bool needs_sending = false;

      // new target
      if (!click_targets_.count(m.first)) needs_sending = true;

      if (!needs_sending) {

        auto* new_target = m.second->getContainingQuads(&empty);
        auto* existing_target = click_targets_.at(m.first)->getContainingQuads(&empty);

        if (new_target->size() != existing_target->size()) needs_sending = true;

        for (unsigned long i=0; i<new_target->size(); i++) {
          if (!needs_sending && 
            *(*new_target)[i] != *(*existing_target)[i])
            needs_sending = true;
        }
        
      }

      if (needs_sending) targets->emplace_back(m.second->clone());
    }
    
    click_targets_ = std::move(click_targets);

    if (targets->size()) {
      auto msg = protocol::PageStream::LayerUpdate::create()
              .setLayerId(layer_id_)
              .build();
      
      msg->setTargets(std::move(targets));
      ins_->GetFrontend()->streamLayerInfo(std::move(msg));
    }
  }

  void commitImage(std::unique_ptr<protocol::PageStream::BufferUpdate> bu) {
    outstanding_images_--;
    
    if (deleted_) return;
    auto buList = std::make_unique<std::vector<std::unique_ptr<protocol::PageStream::BufferUpdate>>>();

    buList->emplace_back(std::move(bu));

    auto msg = protocol::PageStream::LayerUpdate::create()
                  .setLayerId(layer_id_)
                  .setBufferUpdates(std::move(buList))
                  .build();

    ins_->GetFrontend()->streamLayerInfo(std::move(msg));

    if (!outstanding_images_) std::move(callback_).Run();
  }

private:
  scoped_refptr<cc::Layer> layer_;
  int layer_id_;
  std::string layer_as_string_;
  int z_index_;
  bool z_index_changed_;
  int outstanding_images_;
  base::OnceCallback<void()> callback_;
  RegionStateTracker dirty_;

  bool deleted_;
  std::map<int, std::unique_ptr<protocol::PageStream::ClickTarget>> click_targets_;
  WeakPersistent<InspectorPageStreamAgent> ins_;

  std::set<std::pair<int, int>> tile_is_low_res_;

  DISALLOW_COPY_AND_ASSIGN(ClientSideLayer);

};

float InspectorPageStreamAgent::GetDPR() {
  return inspected_frames_->Root()->DevicePixelRatio();          
}

Response InspectorPageStreamAgent::disable() {
  instrumenting_agents_->RemoveInspectorPageStreamAgent(this);
  for (auto l:layers_)
    l.value->Delete();
  layers_.clear();  // Prevents a later UpdateClickTargets callback trying to do anything.

  return Response::OK();
}

void InspectorPageStreamAgent::flush(std::unique_ptr<FlushCallback> cb) {
  flush_callbacks_.push_back(std::move(cb));
  LayerTreeDidChange();
}

void InspectorPageStreamAgent::LayerTreePainted() {
  //GetFrontend()->debugInfo(inspected_frames_->Root()->View()->CompositedLayersAsJSON(static_cast<LayerTreeFlags>(-1))->ToPrettyJSONString());
  //LOG(ERROR) << "LayerTreePainted" << base::debug::StackTrace();;  // 1st

     // GetPropertyTreesJSON(),
     // GetLayerImplJSON()

}

void InspectorPageStreamAgent::LayerRefreshComplete() {
  if (!--pending_frame_refreshs_) {
    if (RootLayer() && RootLayer()->layer_tree_host())
      RootLayer()->layer_tree_host()->StopDeferringCommits(cc::PaintHoldingCommitTrigger::kDisallowed);
    if (GetFrontend()) GetFrontend()->frameDone();
    if (frame_is_queued_) {
      frame_is_queued_ = false;
      LayerTreeDidChange();
    }
    if (!pending_frame_refreshs_ && !frame_is_queued_) {
      for (auto& c : flush_callbacks_) {
        c->sendSuccess();
      }
      flush_callbacks_.clear();
    }
  }
}

void InspectorPageStreamAgent::LayerTreeDidChange() {

  if (!RootLayer() || !RootLayer()->layer_tree_host())
    return;

  // Defer everything till rendering is complete
  RootLayer()->layer_tree_host()->StartDeferringCommits(base::TimeDelta::Max());

  //LOG(ERROR) << "LayerTreeDidChange" << base::debug::StackTrace();;  // 2nd ?

  if (!GetFrontend()) return;
  

  // Set layer order if necessary
  int i=0;
  HashSet<cc::Layer*> layers_in_layer_tree_host;
  for (auto* layer : *(RootLayer()->layer_tree_host())) {
    if (!layers_.Contains(layer))
      layers_.insert(layer, 
        base::MakeRefCounted<ClientSideLayer>(
          layer, this));

    layers_.find(layer)->value->zIndex(i++);

    // keep track of layers that exist.
    layers_in_layer_tree_host.insert(layer);
  }

  // still processing the last layer?  We just record dirty regions and exit.
  // This means our 'defer commits' didn't work...  Instead we will set a flag, and come back
  // to this when we're done with the last frame. 
  if (pending_frame_refreshs_) {
    for (const auto& it : layers_ )
      it.value->MakeDirty();
    frame_is_queued_ = true;
    LOG(ERROR) << "partial-frame";
    return;
  }

  GetFrontend()->frameStart();


  pending_frame_refreshs_++;

  // mark layers that no longer exist as deleted.
  HashSet<cc::Layer*> layers_to_delete;
  for (const auto& it : layers_ ) {
    if (!layers_in_layer_tree_host.Contains(it.key)) {
      it.value->Delete();
      layers_to_delete.insert(it.key);
    };
  }
  for (auto* layer : layers_to_delete )
    layers_.erase(layer);
  

  // Send Prop Trees if necessary
  std::string prop_trees = GetPropertyTreesJSON(RootLayer()->layer_tree_host()->property_trees());
  if (prop_trees_ != prop_trees) {
    prop_trees_ = std::move(prop_trees);
    GetFrontend()->streamPropTrees(Maybe<String>(prop_trees_.c_str()));
  }

  updateClickTargets();

  pending_click_target_update_ = true;

  // Send layer changes if necessary
  //for (auto* layer : RootLayer()->layer_tree_host()->LayersThatShouldPushProperties()){
  for (auto* layer : *(RootLayer()->layer_tree_host())) {
    pending_frame_refreshs_++;
    auto l = layers_.find(layer)->value;

    l->MakeDirty();
    l->Refresh(target_bandwidth_.Get()>0?100:10, 
          base::BindOnce(&InspectorPageStreamAgent::LayerRefreshComplete,
            WrapPersistent(this)));
  }

  LayerRefreshComplete();
}
  
const cc::Layer* InspectorPageStreamAgent::RootLayer() {
  if (inspected_frames_->Root()->View())
    return inspected_frames_->Root()->View()->RootCcLayer();
  else
    return nullptr;
}

std::unique_ptr<protocol::Array<double>> BuildArrayForQuad(
    const FloatQuad& quad) {
  return std::make_unique<std::vector<double>, std::initializer_list<double>>(
      {quad.P1().X(), quad.P1().Y(), quad.P2().X(), quad.P2().Y(),
       quad.P3().X(), quad.P3().Y(), quad.P4().X(), quad.P4().Y()});
}


static std::unique_ptr<protocol::PageStream::ClickTarget> BuildClickTarget(Node* node, cc::Layer** layer) {
  LayoutObject* layout_object = node->GetLayoutObject();
  if (!layout_object) return nullptr;

  const LayoutBoxModelObject& paint_invalidation_container = layout_object->ContainerForPaintInvalidation();
  if (!paint_invalidation_container.Layer())
    return nullptr;

  const PaintLayer& paint_layer = *paint_invalidation_container.Layer();
  GraphicsLayer* gfx_layer = paint_layer.GraphicsLayerBacking(layout_object);
  if (!gfx_layer)
    return nullptr;
  *layer = gfx_layer->CcLayer();
  if (!*layer)
    return nullptr;

  Vector<FloatQuad> abs_quads;
  auto layer_quads = std::make_unique<protocol::Array<protocol::Array<double>>>();
  layout_object->AbsoluteQuads(abs_quads, kTraverseDocumentBoundaries);
  for (FloatQuad& quad : abs_quads) {
    FloatQuad local_quad = paint_invalidation_container.AbsoluteToLocalQuad(quad, kTraverseDocumentBoundaries);
    // The layer might represent a scrollable thing, in which case we want the inner part for coordinates
    if (paint_invalidation_container.GetScrollableArea()) {
      const FloatPoint scroll_position = paint_invalidation_container.GetScrollableArea()->ScrollPosition();
      local_quad.Move(scroll_position.X(), scroll_position.Y());
    }
    layer_quads->emplace_back(BuildArrayForQuad(local_quad));
  }
  return protocol::PageStream::ClickTarget::create()
          .setBackendNodeId(IdentifiersFactory::IntIdForNode(node))
          .setContainingQuads(std::move(layer_quads))
          .build();
}

void InspectorPageStreamAgent::updateClickTargets() {
  if (!send_click_targets_.Get()) return;
  HitTestRequest request(HitTestRequest::kReadOnly | HitTestRequest::kActive |
                         HitTestRequest::kListBased |
                         HitTestRequest::kPenetratingList);
  
  pending_click_target_update_ = false;
  
  if (!inspected_frames_) return;
  auto* root_frame = inspected_frames_->Root();
  auto* root_viewport = root_frame->View()->GetRootFrameViewport();

  PhysicalRect viewport_rect(root_frame->View()->DocumentToFrame(root_viewport->VisibleContentRect()));

  HitTestLocation location(viewport_rect);
  HitTestResult result(request, location);
  root_frame->ContentLayoutObject()->HitTest(location, result);
  std::map<cc::Layer*, std::map<int, std::unique_ptr<protocol::PageStream::ClickTarget>>> click_targets;
  HeapLinkedHashSet<Member<Node>> candidates;
  HeapLinkedHashSet<Member<Node>> excluded;
  Node* previous_node = nullptr;
  for (const auto hit_test_result_node : result.ListBasedTestResult()) {
    Node* node = hit_test_result_node.Get();
    if (!node || node->IsDocumentNode())
      continue;
    if (node->IsPseudoElement() || node->IsTextNode())
      node = node->ParentOrShadowHostNode();
    auto* element = DynamicTo<Element>(node);
    if (!node || node == previous_node || !element)
      continue;
    previous_node = node;
    if (!node->HasEventListeners(event_type_names::kClick) && !node->IsLink())
      continue;
    candidates.insert(node);
    // We exclude all parents of this node, because we don't want to include click handlers inside click handlers
    // They tend to have handlers which look at the event.target JS property, therefore making *any* child node
    // a possible target - that makes way to many nodes.
    while((node = node->ParentOrShadowHostNode()))
      excluded.insert(node);
  }

  for (const auto node : candidates) {
    if (excluded.Contains(node))
      continue;
    cc::Layer* layer;
    std::unique_ptr<protocol::PageStream::ClickTarget> target = BuildClickTarget(node.Get(), &layer);
    if (target) {
      click_targets[layer].emplace(IdentifiersFactory::IntIdForNode(node), std::move(target));
    }
  }

  for (auto& click_targets_for_layer : click_targets) {
    if (layers_.Contains(click_targets_for_layer.first)) {
      scoped_refptr<blink::InspectorPageStreamAgent::ClientSideLayer> layer = 
          layers_.at(click_targets_for_layer.first);

      layer->updateClickTargets(click_targets_for_layer.second);
    }
  }
}

Response InspectorPageStreamAgent::setScroll(int cc_element_id, int x, int y) {
  const auto* root_layer = RootLayer();
  if (!root_layer)
    return Response::OK();

  root_layer->layer_tree_host()->property_trees()->scroll_tree.NotifyDidScroll(cc::ElementId(cc_element_id), gfx::ScrollOffset(x, y), base::nullopt);

  updateClickTargets();

  pending_click_target_update_ = true;

  return Response::OK();
}

Response InspectorPageStreamAgent::clickNode(int backend_node_id) {
  const auto* root_layer = RootLayer();
  if (!root_layer)
    return Response::Error("No root layer");

  Node* node = DOMNodeIds::NodeForId(backend_node_id);
  if (!node)
    return Response::Error("ID does not exist");

  node->GetExecutionContext()
      ->GetTaskRunner(TaskType::kUserInteraction)
      ->PostTask(
          FROM_HERE,
          base::BindOnce(&Node::DispatchSimulatedClick,
                    WrapWeakPersistent(node), nullptr, kSendNoEvents,
                    SimulatedClickCreationScope::kFromUserAgent));

  return Response::OK();
}

}  // namespace blink
