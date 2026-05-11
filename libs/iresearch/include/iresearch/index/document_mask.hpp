#pragma once

#include <absl/container/flat_hash_set.h>

#include <cstddef>

#include "basics/containers/bitset.hpp"
#include "basics/managed_allocator.hpp"
#include "basics/resource_manager.hpp"
#include "iresearch/types.hpp"
#include "iresearch/utils/type_limits.hpp"

namespace irs {

enum class DocumentMaskKind {
  None,
  DeletedHashSet,
  DenseBitset,
};

// Interface for a view of a mask of deleted documents in an index's segment
class DocumentMask {
 public:
  virtual ~DocumentMask() = default;
  virtual bool IsDeleted(doc_id_t doc_id) const = 0;
  virtual size_t DeletedDocCount() const = 0;
  virtual void ForEach(const std::function<void(doc_id_t)>& cb) const = 0;
  virtual DocumentMaskKind Kind() const = 0;

  bool operator==(const DocumentMask& other) const {
    bool equal = (DeletedDocCount() == other.DeletedDocCount());
    ForEach([&equal, &other](doc_id_t doc_id) {
      equal = equal && other.IsDeleted(doc_id);
    });
    return equal;
  }
  bool IsEmpty() const { return DeletedDocCount() == 0; }
  static DocumentMaskKind GetKind(const DocumentMask* mask) {
    return mask ? mask->Kind() : DocumentMaskKind::None;
  }
};

class DocumentHashMask final : public DocumentMask {
 public:
  DocumentHashMask() = default;
  explicit DocumentHashMask(IResourceManager& rm)
    : stored_docs_{ManagedTypedAllocator<doc_id_t>{rm}} {}
  DocumentHashMask(IResourceManager& rm, const DocumentMask& other)
    : stored_docs_{ManagedTypedAllocator<doc_id_t>{rm}} {
    stored_docs_.reserve(other.DeletedDocCount());
    other.ForEach([this](doc_id_t doc_id) { MarkDeleted(doc_id); });
  }
  DocumentHashMask(IResourceManager& rm, size_t /*doc_count*/,
                   const DocumentMask& other)
    : stored_docs_{ManagedTypedAllocator<doc_id_t>{rm}} {
    stored_docs_.reserve(other.DeletedDocCount());
    other.ForEach([this](doc_id_t doc_id) { MarkDeleted(doc_id); });
  }
  DocumentHashMask(IResourceManager& rm, size_t /*doc_count*/,
                   size_t deleted_doc_count)
    : stored_docs_{ManagedTypedAllocator<doc_id_t>{rm}} {
    stored_docs_.reserve(deleted_doc_count);
  }
  bool IsDeleted(doc_id_t doc_id) const override {
    return stored_docs_.contains(doc_id);
  }
  size_t DeletedDocCount() const override { return stored_docs_.size(); }
  void ForEach(const std::function<void(doc_id_t)>& cb) const override {
    for (auto doc_id : stored_docs_) {
      cb(doc_id);
    }
  }
  DocumentMaskKind Kind() const override { return DocumentMaskKind::DeletedHashSet; }

  bool MarkDeleted(doc_id_t doc_id) {
    return stored_docs_.insert(doc_id).second;
  }
  void HintDeletedDocCount(size_t count) { stored_docs_.reserve(count); }
  void Merge(DocumentHashMask& other) {
    stored_docs_.merge(other.stored_docs_);
  }
  void Clear() { stored_docs_.clear(); }

 private:
  absl::flat_hash_set<doc_id_t,
                      absl::container_internal::hash_default_hash<doc_id_t>,
                      absl::container_internal::hash_default_eq<doc_id_t>,
                      ManagedTypedAllocator<doc_id_t>>
    stored_docs_;
};

class DocumentBitMask final : public DocumentMask {
 public:
  DocumentBitMask() = default;
  explicit DocumentBitMask(IResourceManager& rm)
    : is_deleted_{rm}, deleted_doc_count_{0} {}

  DocumentBitMask(IResourceManager& rm, size_t doc_count)
    : is_deleted_{doc_count, rm}, deleted_doc_count_{0} {}
  DocumentBitMask(IResourceManager& rm, size_t doc_count,
                  size_t /*deleted_doc_count*/)
    : is_deleted_{doc_count, rm}, deleted_doc_count_{0} {}

  DocumentBitMask(IResourceManager& rm, size_t doc_count,
                  const DocumentMask& other)
    : is_deleted_{doc_count, rm}, deleted_doc_count_{other.DeletedDocCount()} {
    other.ForEach(
      [this](doc_id_t doc_id) { is_deleted_.set(doc_id - doc_limits::min()); });
  }

  bool IsDeleted(doc_id_t doc_id) const override {
    return is_deleted_.test(doc_id - doc_limits::min());
  }
  size_t DeletedDocCount() const override { return deleted_doc_count_; }
  void ForEach(const std::function<void(doc_id_t)>& cb) const override {
    // NB: doc_id here is 0-based, but user of the class expects valid doc_ids
    // to be 1-based
    auto word_count = is_deleted_.words();
    for (size_t word_idx = 0; word_idx < word_count; ++word_idx) {
      auto word = is_deleted_[word_idx];
      const auto id_base = word_idx * BitsRequired<bitset::word_t>() + doc_limits::min();
      while (word) {
        auto lsb = word & -word;
        cb(id_base + std::countr_zero(lsb));
        word ^= lsb;
      }
    }
  }
  DocumentMaskKind Kind() const override { return DocumentMaskKind::DenseBitset; }

  bool MarkDeleted(doc_id_t doc_id) {
    auto ret = is_deleted_.try_set(doc_id - doc_limits::min());
    deleted_doc_count_ += static_cast<size_t>(ret);
    return ret;
  }

  void Merge(const DocumentMask& other) {
    other.ForEach([this](doc_id_t doc_id) { MarkDeleted(doc_id); });
  }
  void Clear() {
    is_deleted_.clear();
    deleted_doc_count_ = 0;
  }

 private:
  ManagedBitset is_deleted_;
  size_t deleted_doc_count_ = 0;  // to not run popcount each DeletedDocCount call
};

// Stores pointer with tag and runs bounded dispatching of lookups into document
// mask, allowing compiler to inline function calls and prevent full vtable
// lookup.
class DocumentMaskView {
public:
  DocumentMaskView() : mask{nullptr}, kind{DocumentMaskKind::None} {}
  explicit DocumentMaskView(const DocumentMask* mask) : mask{mask}, kind{DocumentMask::GetKind(mask)} {}
  DocumentMaskView(const DocumentMask* mask, DocumentMaskKind kind) : mask{mask}, kind{kind} {
    SDB_ASSERT(DocumentMask::GetKind(mask) == kind);
  }

  bool IsDeleted(doc_id_t doc_id) const {
    switch (kind) {
      case DocumentMaskKind::None:
        return false;
      case DocumentMaskKind::DeletedHashSet:
        return mask &&
               static_cast<const DocumentHashMask*>(mask)->IsDeleted(doc_id);
      case DocumentMaskKind::DenseBitset:
        return mask &&
               static_cast<const DocumentBitMask*>(mask)->IsDeleted(doc_id);
    }
  }
  size_t DeletedDocCount() const {
    return mask ? mask->DeletedDocCount() : 0;
  }
  bool IsEmpty() const {
    return mask ? mask->IsEmpty() : true;
  }
  DocumentMaskKind Kind() const { return kind; }
  const DocumentMask* Mask() const { return mask; }

  bool operator==(const DocumentMaskView& rhs) const {
    if (mask == rhs.mask) {
      return true;
    }
    if ((!mask && rhs.mask->DeletedDocCount() == 0) ||
        (!rhs.mask && mask->DeletedDocCount() == 0)) {
      return true;
    }
    return *mask == *rhs.mask;
  }

private:
  const DocumentMask* mask;
  DocumentMaskKind kind;
};

inline DocumentMaskKind ChooseImmutableRepresentation(
  size_t doc_count, size_t deleted_doc_count) {
  if (deleted_doc_count < doc_count / 100) {  // 0 <= x <1% of documents
    return DocumentMaskKind::DeletedHashSet;
  } else {  // 1 <= x <= 100% of documents
    return DocumentMaskKind::DenseBitset;
  }
}

inline std::shared_ptr<const DocumentMask> BuildImmutableRepresentation(
  IResourceManager& rm, DocumentMaskKind kind, DocumentBitMask&& bit_mask) {
  switch (kind) {
    case DocumentMaskKind::None:
      return nullptr;
    case DocumentMaskKind::DeletedHashSet:
      return std::make_shared<DocumentHashMask>(rm, bit_mask);
    case DocumentMaskKind::DenseBitset:
      return std::make_shared<DocumentBitMask>(std::move(bit_mask));
  }
}

}  // namespace irs
