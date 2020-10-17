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
#include "base/memory/singleton.h"
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
#include "third_party/blink/renderer/platform/widget/frame_widget.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/text/base64.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"
#include "third_party/blink/renderer/platform/wtf/threading_primitives.h"
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
class RegionStateTracker {
public:
  RegionStateTracker() {}

private:
  // if a point is of quality X, then is a member of regions > X
  std::vector<Region> qualities_{kTileQualityHighRes};

public:
  void  AddRect(gfx::Rect a, TileQuality q) {
    for (TileQuality i=kTileQualityDirty; i<kTileQualityHighRes; i++) {
      if (i>=q) {
        // Add to region
        qualities_[i].Unite(Region(IntRect(a)));
      } else {
        // Remove from these regions
        qualities_[i].Subtract(Region(IntRect(a)));
      }
      //qualities_[i].Dump()
    }
  }

  // Finds the rectangle below a given quality within a given region.
  gfx::Rect RectBelowQualityWithinRect(gfx::Rect a, TileQuality q) {

    if (q <= kTileQualityDirty) return gfx::Rect();

    Region res = Intersect(qualities_[q-1], Region(IntRect(a)));

    // We want to return just one rectangle to simplify other stuff
    return res.Bounds();
  }
};

InspectorPageStreamAgent::InspectorPageStreamAgent(
    InspectedFrames* inspected_frames)
    : inspected_frames_(inspected_frames),
    pending_click_target_update_(false), 
    pending_layer_refreshs_(0), 
    layer_refresh_missed_deadline_(false),
    bytes_unacked_(0),
    frames_unacked_(0),
    keyboard_state_(),
    bytes_per_frame_(&agent_state_, /*default_value=*/16000),
    pipeline_frames_(&agent_state_, /*default_value=*/3),
    fps_(&agent_state_, /*default_value=*/60),
    send_click_targets_(&agent_state_, /*default_value=*/true),
    auto_open_click_targets_(&agent_state_, /*default_value=*/false),
    enabled_(&agent_state_, /*default_value=*/false)
     { }

InspectorPageStreamAgent::~InspectorPageStreamAgent() = default;

void InspectorPageStreamAgent::Trace(blink::Visitor* visitor) const {
  visitor->Trace(inspected_frames_);
  InspectorBaseAgent::Trace(visitor);
}

void InspectorPageStreamAgent::Restore() {
  if (enabled_.Get()) {
    // Disable and re-enable to register ourself.
    enabled_.Set(false);
    enable({}, {}, {}, {}, {});
    GetFrontend()->debugInfo("reset");
  }
}

Response InspectorPageStreamAgent::enable(
    Maybe<int> bytes_per_frame,
    Maybe<int> pipeline_frames,
    Maybe<int> fps,
    Maybe<bool> send_click_targets,
    Maybe<bool> auto_open_click_targets) {
  if (!enabled_.Get()) {
    instrumenting_agents_->AddInspectorPageStreamAgent(this);
  
    Document* document = inspected_frames_->Root()->GetDocument();
    if (!document)
      return Response::ServerError("The root frame doesn't have document");

    if (RootLayer()) for (auto* layer : *(RootLayer()->layer_tree_host()))
      layer->SetNeedsDisplay();
  }

  enabled_.Set(true);

  if (bytes_per_frame.isJust()) bytes_per_frame_.Set(bytes_per_frame.fromJust());
  if (pipeline_frames.isJust()) pipeline_frames_.Set(pipeline_frames.fromJust());
  if (fps.isJust()) fps_.Set(fps.fromJust());
  if (send_click_targets.isJust()) send_click_targets_.Set(send_click_targets.fromJust());
  if (auto_open_click_targets.isJust()) auto_open_click_targets_.Set(auto_open_click_targets.fromJust());

  return Response::Success();
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

struct ImageCacheEntry {
  sk_sp<SkSurface> buffer;
  int offsetX;
  int offsetY;

  bool operator<(const ImageCacheEntry &o) const {
      // is the < operator for ponters correct here...??  
      return this->buffer.get() < o.buffer.get() || (this->buffer.get() == o.buffer.get() && std::tie(this->offsetX, this->offsetY) < std::tie(o.offsetX, o.offsetY));
  }
};

class ImageCache {
public:
  ImageCacheEntry InsertAndMatchSurface(sk_sp<SkSurface> s) {
    WTF::MutexLocker locker(mutex_);

    SkPixmap pixmap;
    s->peekPixels(&pixmap);

    std::map<ImageCacheEntry, int> candidates;
    int best_candidate_count = 0;
    ImageCacheEntry best_candidate;


    for (int y = 0; y<s->height()-16-1; y+=16) {
      for (int x = 0; x<s->width()-16-1; x+=16) {
        // each square of image to process

        int bestxi = -1;
        int bestyi = -1;
        uint32_t besthash = 0xFFFFFFFF;

        for (int yi=0; yi<16; yi++){
          for (int xi=0; xi<16; xi++){
            uint32_t hash = 0xFACEB00B ^ *pixmap.addr32(x+xi, y+yi) ^ -*pixmap.addr32(x+xi+1, y+yi+1);
            if (hash == besthash) {
              bestxi = -1;       // uninteresting because it's probably a plain area.
            }
            if (hash < besthash) {
              besthash = hash;
              bestxi = xi;
              bestyi = yi;
            }
          }
        }


        if (bestxi >= 0) {
          auto range = buffers_.equal_range(besthash);
 
          for (auto i = range.first; i != range.second; ++i)
          {
            ImageCacheEntry m{i->second.buffer, i->second.offsetX-bestxi-x, i->second.offsetY-bestyi-y};
            candidates[m]++;
            if (candidates[m]>best_candidate_count) {
              best_candidate = m;
              best_candidate_count = candidates[m];
            }
          }

          buffers_.emplace(besthash, ImageCacheEntry{s, bestxi+x, bestyi+y});
        }
      }
    }

    return best_candidate;

  }

  void Remove(sk_sp<SkSurface> s) {
    WTF::MutexLocker locker(mutex_);

    auto it = buffers_.begin();
    const auto end = buffers_.end();

    while (it != end) {
      if (it->second.buffer == s)
        buffers_.erase(it++);
      else
        ++it;
    }
  }

  static ImageCache* GetInstance() {
    return base::Singleton<ImageCache>::get();
  }

private:
  ImageCache() {}

  friend struct base::DefaultSingletonTraits<ImageCache>;

  WTF::Mutex mutex_;

  std::multimap<uint32_t, ImageCacheEntry> buffers_ GUARDED_BY(mutex_);
  DISALLOW_COPY_AND_ASSIGN(ImageCache);
};

String RenderPicture(sk_sp<SkPicture> input, const gfx::Rect& clip_rect,
                                        double scale, int quality, bool opaque) {
  TRACE_EVENT0("pagestream", "RenderPicture");

  const SkIRect clip = SkIRect::MakeXYWH(clip_rect.x(), clip_rect.y(),
                                     clip_rect.width(),
                                     clip_rect.height());
  
  int width = ceil(scale * clip.width());
  int height = ceil(scale * clip.height());

  sk_sp<SkSurface> surface = SkSurface::MakeRasterN32Premul(width, height);
  SkCanvas* canvas = surface->getCanvas();
  
  canvas->scale(scale, scale);
  canvas->translate(-clip_rect.x(), -clip_rect.y());
  
  {
    TRACE_EVENT0("pagestream", "RenderPicture-playback");
    input->playback(canvas, nullptr);
  }

  ImageCacheEntry match = ImageCache::GetInstance()->InsertAndMatchSurface(surface);
  
  

  sk_sp<SkImage> img(surface->makeImageSnapshot());
  if (!img) return "";

  sk_sp<SkData> webp;
  {
    TRACE_EVENT0("pagestream", "RenderPicture-encode");
    webp = img->encodeToData(SkEncodedImageFormat::kWEBP, quality);
    if (!webp) return "";
  }
  
  TRACE_EVENT0("pagestream", "RenderPicture-base64");
  return "data:image/webp;base64," + Base64Encode(base::span<const uint8_t>(webp->bytes(), webp->size()));
}

void RenderPictureAndPostResult(scoped_refptr<InspectorTaskRunner> task_runner,
                                sk_sp<SkPicture> input, std::unique_ptr<gfx::Rect> clip_rect, float scale,
                                float quality_factor, bool opaque, base::TimeTicks deadline,
                                WTF::CrossThreadOnceFunction<void(std::unique_ptr<protocol::PageStream::BufferUpdate>)> result_callback) {
  TRACE_EVENT0("pagestream", "RenderPictureAndPostResult");

  std::unique_ptr<protocol::PageStream::BufferUpdate> buf_msg;
  if (base::TimeTicks::Now() < deadline) {
    // within deadline. 

    String imagedata = RenderPicture(input, *clip_rect, scale, quality_factor, opaque);

    buf_msg = protocol::PageStream::BufferUpdate::create()
      .setImage(imagedata.IsolatedCopy())
      .setClip(BuildObjectForRect(*clip_rect))
      .build();
  }

  // Will interrupt javascript on main thread.
  task_runner->AppendTask(CrossThreadBindOnce(std::move(result_callback), std::move(buf_msg)));
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
    outstanding_tiles_(0),
    callback_(),
    dirty_(),
    bytes_sent_(0),
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
    newly_dirty_rects_.push_back(layer_->update_rect());
    pic_ = nullptr;
  }

  void Refresh(TileQuality layer_quality, base::TimeTicks deadline, base::OnceCallback<void(bool, int)> callback) {
    TRACE_EVENT0("pagestream", "ClientSideLayer::Refresh");
    DCHECK(callback_.is_null());
    callback_ = std::move(callback);
    last_refresh_complete_ = true;
    bytes_sent_ = 0;

    auto msg = protocol::PageStream::LayerUpdate::create()
                  .setLayerId(layer_id_)
                  .build();

    std::string new_layer_as_string(layer_->ToString());
    if (layer_as_string_ != new_layer_as_string) {
      layer_as_string_ = std::move(new_layer_as_string);
      msg->setLayerInfo(layer_as_string_.c_str());
    }
    
    gfx::Rect bounds(layer_->bounds());

    gfx::Rect visible_region = GetVisibleRect(layer_.get());
    gfx::Rect nearly_visible_region(visible_region);
    nearly_visible_region.Inset(-visible_region.width(), -visible_region.height());


    // Mark everything dirty since the last run.
    for (const auto& r : newly_dirty_rects_)
      dirty_.AddRect(r, kTileQualityDirty);

    newly_dirty_rects_.clear();


#define CELL_SIZE 256
    for (int cell_x=0; cell_x<=(bounds.width()-1)/CELL_SIZE; cell_x++) {
      for (int cell_y=0; cell_y<=(bounds.height()-1)/CELL_SIZE; cell_y++) {
        TRACE_EVENT2("pagestream", "ClientSideLayer::Refresh::processingTile", "tilex", cell_x, "tiley", cell_y);
        
        gfx::Rect tile_rect(cell_x*CELL_SIZE, cell_y*CELL_SIZE, CELL_SIZE, CELL_SIZE);

        TileQuality desired_quality = layer_quality;
        
        if (!visible_region.Intersects(tile_rect)) desired_quality--;
        if (!nearly_visible_region.Intersects(tile_rect)) desired_quality--;
        
        auto clip_rect = dirty_.RectBelowQualityWithinRect(tile_rect, desired_quality);

        if (clip_rect.IsEmpty()) continue;

        // For testing
        //clip_rect = tile_rect;
        //desired_quality = kTileQualityHighRes;

        float scale, quality_factor;
        switch (desired_quality) {
          case kTileQualityHighRes:
            scale = 1.0;
            quality_factor = 10;
            break;
          case kTileQualityLowRes:
            scale = 1.0/8;
            quality_factor = 10;
            break;
          default:
            continue;
        }

        scale *= ins_->GetDPR();

        {
          TRACE_EVENT0("pagestream", "ClientSideLayer::Refresh::gettingPicture");
          if (!pic_) pic_ = layer_->GetPicture();
          if (!pic_) continue;
        }

        
        LOG(ERROR) << "Starting render.  Layer: " << layer_id_ << " X:" << cell_x << " Y:" << cell_y << " Quality:" << (int)desired_quality << " Opaque:" << layer_->contents_opaque();
        outstanding_tiles_++;

        {
          TRACE_EVENT0("pagestream", "ClientSideLayer::Refresh::postingTask");
        
          worker_pool::PostTask(
              FROM_HERE, {base::TaskPriority::BEST_EFFORT},
              CrossThreadBindOnce(
                  RenderPictureAndPostResult, ins_->GetTaskRunner(),
                  pic_, std::make_unique<gfx::Rect>(clip_rect), scale, quality_factor, layer_->contents_opaque(),
                  deadline, 
                  CrossThreadBindOnce(&InspectorPageStreamAgent::ClientSideLayer::commitImage,
                                      WrapRefCounted(this), desired_quality, std::make_unique<gfx::Rect>(clip_rect))));
        }
      }
    }

    if (z_index_changed_) {
      msg->setZIndex(z_index_);
      z_index_changed_ = false;
    }

    if (msg->hasZIndex() || msg->hasTargets() || msg->hasBufferUpdates() || msg->hasLayerInfo())
      ins_->GetFrontend()->streamLayerInfo(std::move(msg));

    if (!outstanding_tiles_) std::move(callback_).Run(true, 0);
  }

  void zIndex(int z) {
    if (z_index_==z) return;
    z_index_changed_ = true;
    z_index_ = z;
  }

  int zIndex() { return z_index_; }

  void updateClickTargets(std::map<int, std::unique_ptr<protocol::PageStream::ClickTarget>>& click_targets) {
    std::unique_ptr<protocol::Array<protocol::PageStream::ClickTarget>> targets = std::make_unique<protocol::Array<protocol::PageStream::ClickTarget>>();
    TRACE_EVENT0("pagestream", "updateClickTargets");
    
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

  void commitImage(TileQuality quality, std::unique_ptr<gfx::Rect> clip_rect, std::unique_ptr<protocol::PageStream::BufferUpdate> bu) {
    TRACE_EVENT0("pagestream", "commitImage");

    outstanding_tiles_--;
      
    if (deleted_) return;

    if (bu) {
      bytes_sent_ += bu->getImage("").length();

      //LOG(ERROR) << "Done render.  Layer: " << layer_id_ << " Quality:" << (int)quality << " Bytes:" << bu->getImage("").length();
      
      auto buList = std::make_unique<std::vector<std::unique_ptr<protocol::PageStream::BufferUpdate>>>();

      buList->emplace_back(std::move(bu));

      auto msg = protocol::PageStream::LayerUpdate::create()
                    .setLayerId(layer_id_)
                    .setBufferUpdates(std::move(buList))
                    .build();

      ins_->GetFrontend()->streamLayerInfo(std::move(msg));

      dirty_.AddRect(*clip_rect, quality);
    } else {
      last_refresh_complete_ = false;
      //LOG(ERROR) << "Fail render.";
      
    }

    if (!outstanding_tiles_) std::move(callback_).Run(last_refresh_complete_, bytes_sent_);
  }

private:
  scoped_refptr<cc::Layer> layer_;
  int layer_id_;
  std::string layer_as_string_;
  int z_index_;
  bool z_index_changed_;
  int outstanding_tiles_;
  base::OnceCallback<void(bool, int)> callback_;
  RegionStateTracker dirty_;
  // Regions that became dirty during the currently being processed frame.
  std::vector<gfx::Rect> newly_dirty_rects_;
  bool last_refresh_complete_;
  int bytes_sent_;

  bool deleted_;
  sk_sp<SkPicture> pic_;
  std::map<int, std::unique_ptr<protocol::PageStream::ClickTarget>> click_targets_;
  WeakPersistent<InspectorPageStreamAgent> ins_;

  DISALLOW_COPY_AND_ASSIGN(ClientSideLayer);

};

float InspectorPageStreamAgent::GetDPR() {
  return inspected_frames_->Root()->DevicePixelRatio();          
}

scoped_refptr<InspectorTaskRunner> InspectorPageStreamAgent::GetTaskRunner() {
  return inspected_frames_->Root()->GetInspectorTaskRunner();
}

Response InspectorPageStreamAgent::disable() {
  if (enabled_.Get()) {
    instrumenting_agents_->RemoveInspectorPageStreamAgent(this);
    for (auto l:layers_)
      l.value->Delete();
    layers_.clear();  // Prevents a later UpdateClickTargets callback trying to do anything.
    enabled_.Set(false);
  }
  return Response::Success();
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

void InspectorPageStreamAgent::LayerRefreshComplete(bool all_done, int bytes_sent) {
  TRACE_EVENT0("pagestream", "LayerRefreshComplete");

  // We queue another frame refresh if deadlines were missed in tile rendering.
  bytes_unacked_ += bytes_sent;
  if (!all_done) layer_refresh_missed_deadline_ = true;
  if (!--pending_layer_refreshs_) {
    if (RootLayer() && RootLayer()->layer_tree_host())
      RootLayer()->layer_tree_host()->StopDeferringCommits(cc::PaintHoldingCommitTrigger::kDisallowed);
    if (GetFrontend()) GetFrontend()->frameDone();

    LOG(ERROR) << 
        "bytes sent this frame: " << bytes_unacked_ <<
        " quality: " << (int)layer_refresh_quality_ <<
        " incomplete: " << layer_refresh_missed_deadline_;

    bytes_unacked_ = 0; // -= bytes_per_frame_.Get();
    frames_unacked_++;

    if (frame_is_queued_ || layer_refresh_missed_deadline_ || layer_refresh_quality_ != kTileQualityHighRes) {
      // Just reschedule the layer refreshes without any invalidation
      LayerTreeDidChangeInternal(true);   // Might be reentrant
    } else {
      for (auto& c : flush_callbacks_) {
        c->sendSuccess();
      }
      flush_callbacks_.clear();
    }
  }
}

Response InspectorPageStreamAgent::ackFrame() {
  frames_unacked_--;
  if (!frames_unacked_ || bytes_unacked_<0) {
    bytes_unacked_ = 0;
  }

  return Response::Success();
}


void InspectorPageStreamAgent::LayerTreeDidChange() {
  LayerTreeDidChangeInternal(false);
}

void InspectorPageStreamAgent::LayerTreeDidChangeInternal(bool no_dirty) {
  TRACE_EVENT0("pagestream", "LayerTreeDidChangeInternal");


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
  if (pending_layer_refreshs_) {
    for (const auto& it : layers_ )
      if (!no_dirty) 
        it.value->MakeDirty();
    frame_is_queued_ = true;
    LOG(ERROR) << "partial-frame";
    return;
  }

  frame_is_queued_ = false;
  
  GetFrontend()->frameStart();


  pending_layer_refreshs_++;

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
  updateKeyboard();

  pending_click_target_update_ = true;

  if (layer_refresh_missed_deadline_ || !no_dirty) {
    layer_refresh_quality_ = kTileQualityLowRes;
  } else {
    layer_refresh_quality_ = kTileQualityHighRes;
  }
  layer_refresh_missed_deadline_ = false;

  // Send layer changes if necessary
  //for (auto* layer : RootLayer()->layer_tree_host()->LayersThatShouldPushProperties()){
  for (auto* layer : *(RootLayer()->layer_tree_host())) {
    pending_layer_refreshs_++;
    auto l = layers_.find(layer)->value;

    if (!no_dirty) 
      l->MakeDirty();
    l->Refresh(layer_refresh_quality_, base::TimeTicks::Now() + base::TimeDelta::FromSecondsD(1/60.0),
          base::BindOnce(&InspectorPageStreamAgent::LayerRefreshComplete,
            WrapPersistent(this)));
  }
  LayerRefreshComplete(true, 0);
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

  const LayoutBoxModelObject& paint_invalidation_container = layout_object->DirectlyCompositableContainer();
  if (!paint_invalidation_container.Layer())
    return nullptr;

  const PaintLayer& paint_layer = *paint_invalidation_container.Layer();
  GraphicsLayer* gfx_layer = paint_layer.GraphicsLayerBacking(layout_object);
  if (!gfx_layer)
    return nullptr;
  *layer = &gfx_layer->CcLayer();
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

void InspectorPageStreamAgent::updateKeyboard() {
  const auto& new_keyboard_state = inspected_frames_->Root()->GetWidgetForLocalRoot()->TextInputInfo();

  if (!keyboard_state_.Equals(new_keyboard_state)) {
    keyboard_state_ = new_keyboard_state;

    GetFrontend()->keyboardStateChange(
      keyboard_state_.type != kWebTextInputTypeNone /* showing */,
      keyboard_state_.value,
      keyboard_state_.selection_start,
      keyboard_state_.selection_end
    );
  }
}

Response InspectorPageStreamAgent::setKeyboardState(const String& input_box_value, double selection_start, double selection_end) {

  FrameWidget* fw = inspected_frames_->Root()->GetWidgetForLocalRoot();

  fw->SetComposition(input_box_value, Vector<ui::ImeTextSpan>(), gfx::Range(0, 999), (int)selection_start, (int)selection_end);
  
  return Response::Success();
}

void InspectorPageStreamAgent::updateClickTargets() {
  if (!send_click_targets_.Get()) return;
  HitTestRequest request(HitTestRequest::kReadOnly | HitTestRequest::kActive |
                         HitTestRequest::kIgnorePointerEventsNone |
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
  for (const auto& hit_test_result_node : result.ListBasedTestResult()) {
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

    /*  TODO:  Fix this code to ignore child elements of Node, and make it vaguely performant.
    // If this element is occluded partially, skip it.
    HitTestResult occlusion_result = node->GetLayoutObject()->HitTestForOcclusion();
    if (result.InnerNode() != node)
      continue;
    */

    candidates.insert(node);
    // We exclude all parents of this node, because we don't want to include click handlers inside click handlers
    // They tend to have handlers which look at the event.target JS property, therefore making *any* child node
    // a possible target - that makes way to many nodes.
    while((node = node->ParentOrShadowHostNode()))
      excluded.insert(node);
  }

  for (const auto& node : candidates) {
    if (excluded.Contains(node))
      continue;
    cc::Layer* layer;
    std::unique_ptr<protocol::PageStream::ClickTarget> target = BuildClickTarget(node.Get(), &layer);
    if (target) {
      click_targets[layer].emplace(IdentifiersFactory::IntIdForNode(node), std::move(target));
    }
  }

  for (const auto& it : layers_ ) {
    it.value->updateClickTargets(click_targets[it.key]);
  }
}

Response InspectorPageStreamAgent::setScroll(int cc_element_id, int x, int y) {
  const auto* root_layer = RootLayer();
  if (!root_layer)
    return Response::Success();

  root_layer->layer_tree_host()->property_trees()->scroll_tree.NotifyDidScroll(cc::ElementId(cc_element_id), gfx::ScrollOffset(x, y), base::nullopt);

  updateClickTargets();

  pending_click_target_update_ = true;

  return Response::Success();
}

Response InspectorPageStreamAgent::clickNode(int backend_node_id) {
  const auto* root_layer = RootLayer();
  if (!root_layer)
    return Response::ServerError("No root layer");

  Node* node = DOMNodeIds::NodeForId(backend_node_id);
  if (!node)
    return Response::InvalidParams("ID does not exist");

  node->GetExecutionContext()
      ->GetTaskRunner(TaskType::kUserInteraction)
      ->PostTask(
          FROM_HERE,
          base::BindOnce(&Node::DispatchSimulatedClick,
                    WrapWeakPersistent(node), nullptr, kSendNoEvents,
                    SimulatedClickCreationScope::kFromUserAgent));

  return Response::Success();
}

}  // namespace blink
