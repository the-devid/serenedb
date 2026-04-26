#pragma once


#include "basics/managed_allocator.hpp"
#include "basics/resource_manager.hpp"
#include "basics/containers/bitset.hpp"
#include "iresearch/types.hpp"
#include "iresearch/utils/type_limits.hpp"

#include <absl/container/flat_hash_set.h>

#include <cstddef>

namespace irs {

// Interface for a view of a mask of deleted documents in an index's segment
class DocumentMask {
 public:
  virtual ~DocumentMask() = default;
  virtual bool IsDeleted(doc_id_t doc_id) const = 0;
  virtual size_t DeletedDocCount() const = 0;
  virtual void ForEach(const std::function<void(doc_id_t)>& cb) const = 0;

  bool operator==(const DocumentMask& other) const {
    bool equal = (DeletedDocCount() == other.DeletedDocCount());
    ForEach([&equal, &other](doc_id_t doc_id) {
        equal = equal && other.IsDeleted(doc_id);
    });
    return equal;
  }
  bool IsEmpty() const {
    return DeletedDocCount() == 0;
  }
};


class DocumentHashMask final : public DocumentMask {
 public:
  DocumentHashMask() = default;
  explicit DocumentHashMask(IResourceManager& rm) : stored_docs_{ManagedTypedAllocator<doc_id_t>{rm}} {}
  DocumentHashMask(IResourceManager& rm, const DocumentMask& other) : stored_docs_{ManagedTypedAllocator<doc_id_t>{rm}} {
    HintDeletedDocCount(other.DeletedDocCount());
    other.ForEach([this](doc_id_t doc_id) {
      MarkDeleted(doc_id);
    });
  }

  bool IsDeleted(doc_id_t doc_id) const override {
      return stored_docs_.contains(doc_id);
  }
  size_t DeletedDocCount() const override {
      return stored_docs_.size();
  }
  void ForEach(const std::function<void(doc_id_t)>& cb) const override {
    for (auto doc_id : stored_docs_) {
      cb(doc_id);
    }
  }

  bool MarkDeleted(doc_id_t doc_id) {
    return stored_docs_.insert(doc_id).second;
  }
  void HintDeletedDocCount(size_t count) {
    stored_docs_.reserve(count);
  }
  void Merge(DocumentHashMask& other) {
    stored_docs_.merge(other.stored_docs_);
  }
  void Clear() {
    stored_docs_.clear();
  }

 private:
  absl::flat_hash_set<doc_id_t,
    absl::container_internal::hash_default_hash<doc_id_t>,
    absl::container_internal::hash_default_eq<doc_id_t>,
    ManagedTypedAllocator<doc_id_t>> stored_docs_;
};

}  // namespace irs
