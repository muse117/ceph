// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/Utils.h"
#include "common/dout.h"
#include "include/buffer.h"
#include "include/rados/librados.hpp"
#include "include/neorados/RADOS.hpp"
#include "librbd/internal.h"
#include "librbd/Utils.h"
#include "librbd/io/AioCompletion.h"
#include "librbd/io/ImageDispatchSpec.h"
#include "osd/osd_types.h"
#include "osdc/Striper.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::io::util: " << __func__ << ": "

namespace librbd {
namespace io {
namespace util {

void apply_op_flags(uint32_t op_flags, uint32_t flags, neorados::Op* op) {
  if (op_flags & LIBRADOS_OP_FLAG_FADVISE_RANDOM)
    op->set_fadvise_random();
  if (op_flags & LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL)
    op->set_fadvise_sequential();
  if (op_flags & LIBRADOS_OP_FLAG_FADVISE_WILLNEED)
    op->set_fadvise_willneed();
  if (op_flags & LIBRADOS_OP_FLAG_FADVISE_DONTNEED)
    op->set_fadvise_dontneed();
  if (op_flags & LIBRADOS_OP_FLAG_FADVISE_NOCACHE)
    op->set_fadvise_nocache();

  if (flags & librados::OPERATION_BALANCE_READS)
    op->balance_reads();
  if (flags & librados::OPERATION_LOCALIZE_READS)
    op->localize_reads();
}

bool assemble_write_same_extent(
    const LightweightObjectExtent &object_extent, const ceph::bufferlist& data,
    ceph::bufferlist *ws_data, bool force_write) {
  size_t data_len = data.length();

  if (!force_write) {
    bool may_writesame = true;
    for (auto& q : object_extent.buffer_extents) {
      if (!(q.first % data_len == 0 && q.second % data_len == 0)) {
        may_writesame = false;
        break;
      }
    }

    if (may_writesame) {
      ws_data->append(data);
      return true;
    }
  }

  for (auto& q : object_extent.buffer_extents) {
    bufferlist sub_bl;
    uint64_t sub_off = q.first % data_len;
    uint64_t sub_len = data_len - sub_off;
    uint64_t extent_left = q.second;
    while (extent_left >= sub_len) {
      sub_bl.substr_of(data, sub_off, sub_len);
      ws_data->claim_append(sub_bl);
      extent_left -= sub_len;
      if (sub_off) {
	sub_off = 0;
	sub_len = data_len;
      }
    }
    if (extent_left) {
      sub_bl.substr_of(data, sub_off, extent_left);
      ws_data->claim_append(sub_bl);
    }
  }
  return false;
}

template <typename I>
void read_parent(I *image_ctx, uint64_t object_no, ReadExtents* extents,
                 librados::snap_t snap_id, const ZTracer::Trace &trace,
                 Context* on_finish) {

  auto cct = image_ctx->cct;

  std::shared_lock image_locker{image_ctx->image_lock};

  // calculate reverse mapping onto the image
  Extents parent_extents;
  for (auto& extent: *extents) {
    Striper::extent_to_file(cct, &image_ctx->layout, object_no, extent.offset,
                            extent.length, parent_extents);
  }

  uint64_t parent_overlap = 0;
  uint64_t object_overlap = 0;
  int r = image_ctx->get_parent_overlap(snap_id, &parent_overlap);
  if (r == 0) {
    object_overlap = image_ctx->prune_parent_extents(parent_extents,
                                                     parent_overlap);
  }

  if (object_overlap == 0) {
    image_locker.unlock();

    on_finish->complete(-ENOENT);
    return;
  }

  ldout(cct, 20) << dendl;

  ceph::bufferlist* parent_read_bl;
  if (extents->size() > 1) {
    auto parent_comp = new ReadResult::C_ObjectReadMergedExtents(
            cct, extents, on_finish);
    parent_read_bl = &parent_comp->bl;
    on_finish = parent_comp;
  } else {
    parent_read_bl = &extents->front().bl;
  }

  auto comp = AioCompletion::create_and_start(on_finish, image_ctx->parent,
                                              AIO_TYPE_READ);
  ldout(cct, 20) << "completion " << comp << ", extents " << parent_extents
                 << dendl;
  auto req = io::ImageDispatchSpec::create_read(
    *image_ctx->parent, io::IMAGE_DISPATCH_LAYER_INTERNAL_START, comp,
    std::move(parent_extents), ReadResult{parent_read_bl},
    image_ctx->parent->get_data_io_context(), 0, 0, trace);
  req->send();
}

template <typename I>
int clip_request(I *image_ctx, Extents *image_extents) {
  std::shared_lock image_locker{image_ctx->image_lock};
  for (auto &image_extent : *image_extents) {
    auto clip_len = image_extent.second;
    int r = clip_io(librbd::util::get_image_ctx(image_ctx),
                    image_extent.first, &clip_len);
    if (r < 0) {
      return r;
    }

    image_extent.second = clip_len;
  }
  return 0;
}

void unsparsify(CephContext* cct, ceph::bufferlist* bl,
                const Extents& extent_map, uint64_t bl_off,
                uint64_t out_bl_len) {
  Striper::StripedReadResult destriper;
  bufferlist out_bl;

  destriper.add_partial_sparse_result(cct, std::move(*bl), extent_map, bl_off,
                                      {{0, out_bl_len}});
  destriper.assemble_result(cct, out_bl, true);
  *bl = out_bl;
}

} // namespace util
} // namespace io
} // namespace librbd

template void librbd::io::util::read_parent(
    librbd::ImageCtx *image_ctx, uint64_t object_no, ReadExtents* extents,
    librados::snap_t snap_id, const ZTracer::Trace &trace, Context* on_finish);
template int librbd::io::util::clip_request(
    librbd::ImageCtx *image_ctx, Extents *image_extents);
