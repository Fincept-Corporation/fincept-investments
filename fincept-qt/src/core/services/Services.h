#pragma once
// Phase 9 scaffolding — explicit, named service container.
//
// The codebase currently has 40+ `::instance()` singletons. Each carries two
// hidden costs:
//   - static-init order across translation units is undefined
//   - tests cannot substitute alternative implementations
//
// `Services` is a single explicit object constructed in `main.cpp` after the
// logger but before any subsystem touches another. Each accessor returns the
// current canonical instance. Existing `Foo::instance()` calls keep working
// during migration — they delegate here.
//
// **Do not** add a new `::instance()` singleton. New cross-cutting services
// belong here.

namespace fincept {

class EventBus;
class Database;
class SecureStorage;

namespace datahub {
class DataHub;
} // namespace datahub

/// Single explicit container for cross-cutting infrastructure.
/// Constructed once in `main.cpp`. Accessed via `Services::root()`.
///
/// This is a *façade* during the migration: each accessor returns the existing
/// `Foo::instance()` for now. As individual singletons are migrated, the
/// implementations are replaced with owned members.
class Services {
  public:
    /// The process-global instance. Constructed lazily on first access; that
    /// matches the existing singleton lifetime so wiring stays stable.
    static Services& root();

    // -- Accessors (façade over existing singletons during migration) ----------
    EventBus& events();
    Database& db();
    SecureStorage& secure_storage();
    datahub::DataHub& hub();

  private:
    Services() = default;
    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;
};

} // namespace fincept
