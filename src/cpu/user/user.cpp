#include <cpu/user/user.hpp>
#include <common/string.hpp>
#include <memory/pmm.hpp>

UserManager& UserManager::get() {
    static UserManager userManagerInstance;
    return userManagerInstance;
}

uint64_t UserManager::hashPassword(const char* password) {
    if (!password) return 0;

    uint64_t hash = 5381;
    int c;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(password);
    while ((c = *p++)) {
        hash = ((hash << 5) + hash) ^ static_cast<uint64_t>(c);
    }
    p = reinterpret_cast<const unsigned char*>(password);
    while ((c = *p++)) {
        hash = ((hash << 7) + hash) ^ static_cast<uint64_t>(c);
    }
    return hash;
}

void UserManager::initialize() {
    if (initialized) return;

    for (uint32_t i = 0; i < MAX_USERS; i++) {
        users[i].valid  = false;
        users[i].locked = false;
    }

    nextUID    = FIRST_USER_UID;
    userCount  = 0;
    initialized = true;

    createRootUser();
}

void UserManager::createRootUser() {
    User& root = users[0];
    root.uid          = ROOT_UID;
    root.gid          = ROOT_GID;
    root.passwordHash = hashPassword("root");
    root.locked       = false;
    root.valid        = true;
    strncpy(root.username, "root",   MAX_USERNAME_LEN - 1);
    strncpy(root.homeDir,  "/root",  MAX_HOME_LEN   - 1);
    strncpy(root.shell,    "/shell", MAX_SHELL_LEN  - 1);
    root.username[MAX_USERNAME_LEN - 1] = '\0';
    root.homeDir [MAX_HOME_LEN    - 1] = '\0';
    root.shell   [MAX_SHELL_LEN   - 1] = '\0';
    userCount++;
}

bool UserManager::uidExists(uint32_t uid) const {
    for (uint32_t i = 0; i < MAX_USERS; i++) {
        if (users[i].valid && users[i].uid == uid) return true;
    }
    return false;
}

bool UserManager::nameExists(const char* username) const {
    if (!username) return false;
    for (uint32_t i = 0; i < MAX_USERS; i++) {
        if (users[i].valid && strncmp(users[i].username, username, MAX_USERNAME_LEN) == 0)
            return true;
    }
    return false;
}

int UserManager::findFreeSlot() const {
    for (uint32_t i = 0; i < MAX_USERS; i++) {
        if (!users[i].valid) return static_cast<int>(i);
    }
    return -1;
}

bool UserManager::addUser(const char* username,
                          const char* password,
                          uint32_t    uid,
                          uint32_t    gid,
                          const char* homeDir,
                          const char* shell) {
    if (!initialized || !username || !password) return false;

    if (uid == INVALID_UID) {
        uid = allocateUID();
    }

    if (uidExists(uid))    return false;
    if (nameExists(username)) return false;

    int slot = findFreeSlot();
    if (slot < 0) return false;

    User& u = users[slot];
    u.uid          = uid;
    u.gid          = gid;
    u.passwordHash = hashPassword(password);
    u.locked       = false;
    u.valid        = true;

    strncpy(u.username, username, MAX_USERNAME_LEN - 1);
    u.username[MAX_USERNAME_LEN - 1] = '\0';

    if (homeDir) {
        strncpy(u.homeDir, homeDir, MAX_HOME_LEN - 1);
        u.homeDir[MAX_HOME_LEN - 1] = '\0';
    } else {
        u.homeDir[0] = '\0';
    }

    if (shell) {
        strncpy(u.shell, shell, MAX_SHELL_LEN - 1);
        u.shell[MAX_SHELL_LEN - 1] = '\0';
    } else {
        strncpy(u.shell, "/shell", MAX_SHELL_LEN - 1);
        u.shell[MAX_SHELL_LEN - 1] = '\0';
    }

    userCount++;
    return true;
}

bool UserManager::removeUser(uint32_t uid) {
    if (!initialized) return false;
    if (uid == ROOT_UID) return false;

    for (uint32_t i = 0; i < MAX_USERS; i++) {
        if (users[i].valid && users[i].uid == uid) {
            users[i].valid = false;
            userCount--;
            return true;
        }
    }
    return false;
}

bool UserManager::lockUser(uint32_t uid) {
    User* u = getUserByUID(uid);
    if (!u) return false;
    u->locked = true;
    return true;
}

bool UserManager::unlockUser(uint32_t uid) {
    User* u = getUserByUID(uid);
    if (!u) return false;
    u->locked = false;
    return true;
}

bool UserManager::setPassword(uint32_t uid, const char* newPassword) {
    if (!newPassword) return false;
    User* u = getUserByUID(uid);
    if (!u) return false;
    u->passwordHash = hashPassword(newPassword);
    return true;
}

User* UserManager::getUserByUID(uint32_t uid) {
    for (uint32_t i = 0; i < MAX_USERS; i++) {
        if (users[i].valid && users[i].uid == uid) return &users[i];
    }
    return nullptr;
}

User* UserManager::getUserByName(const char* username) {
    if (!username) return nullptr;
    for (uint32_t i = 0; i < MAX_USERS; i++) {
        if (users[i].valid &&
            strncmp(users[i].username, username, MAX_USERNAME_LEN) == 0) {
            return &users[i];
        }
    }
    return nullptr;
}

uint32_t UserManager::getUIDByName(const char* username) {
    User* u = getUserByName(username);
    return u ? u->uid : INVALID_UID;
}

User* UserManager::getUserByIndex(uint32_t idx) {
    if (idx >= MAX_USERS) return nullptr;
    return users[idx].valid ? &users[idx] : nullptr;
}

uint32_t UserManager::getUserCount() const {
    return userCount;
}

bool UserManager::authenticate(const char* username, const char* password) {
    if (!username || !password) return false;
    User* u = getUserByName(username);
    if (!u || u->locked) return false;
    return u->passwordHash == hashPassword(password);
}

bool UserManager::authenticateUID(uint32_t uid, const char* password) {
    if (!password) return false;
    User* u = getUserByUID(uid);
    if (!u || u->locked) return false;
    return u->passwordHash == hashPassword(password);
}

uint32_t UserManager::allocateUID() {
    uint32_t candidate = nextUID;
    for (uint32_t tries = 0; tries < (0xFFFFFFFE - FIRST_USER_UID); tries++) {
        if (candidate == INVALID_UID) { candidate = FIRST_USER_UID; }
        if (!uidExists(candidate)) {
            nextUID = candidate + 1;
            return candidate;
        }
        candidate++;
    }
    return INVALID_UID;
}