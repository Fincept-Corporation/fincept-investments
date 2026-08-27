#include "core/services/Services.h"

#include "core/events/EventBus.h"
#include "datahub/DataHub.h"
#include "storage/secure/SecureStorage.h"
#include "storage/sqlite/Database.h"

namespace fincept {

Services& Services::root() {
    static Services s;
    return s;
}

EventBus& Services::events() {
    return EventBus::instance();
}
Database& Services::db() {
    return Database::instance();
}
SecureStorage& Services::secure_storage() {
    return SecureStorage::instance();
}
datahub::DataHub& Services::hub() {
    return datahub::DataHub::instance();
}

} // namespace fincept
