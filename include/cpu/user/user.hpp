#pragma once

#include <stdint.h>
#include <stddef.h>

constexpr uint32_t MAX_USERS        = 32;
constexpr uint32_t ROOT_UID         = 0;
constexpr uint32_t ROOT_GID         = 0;
constexpr uint32_t INVALID_UID      = 0xFFFFFFFF;
constexpr uint32_t INVALID_GID      = 0xFFFFFFFF;
constexpr uint32_t FIRST_USER_UID   = 1000;
constexpr size_t   MAX_USERNAME_LEN = 32;
constexpr size_t   MAX_HOME_LEN     = 256;
constexpr size_t   MAX_SHELL_LEN    = 256;

struct User {
    uint32_t uid;
    uint32_t gid;
    char     username[MAX_USERNAME_LEN];
    uint64_t passwordHash;
    char     homeDir[MAX_HOME_LEN];
    char     shell[MAX_SHELL_LEN];
    bool     valid;
    bool     locked;
};

class UserManager {
public:
    static UserManager& get();

    void initialize();

    bool addUser(const char* username,
                 const char* password,
                 uint32_t    uid,
                 uint32_t    gid,
                 const char* homeDir,
                 const char* shell);

    bool removeUser(uint32_t uid);

    bool lockUser(uint32_t uid);
    bool unlockUser(uint32_t uid);

    bool setPassword(uint32_t uid, const char* newPassword);

    User*    getUserByUID(uint32_t uid);
    User*    getUserByName(const char* username);
    uint32_t getUIDByName(const char* username);

    User* getUserByIndex(uint32_t idx);
    uint32_t getUserCount() const;

    bool authenticate(const char* username, const char* password);
    bool authenticateUID(uint32_t uid, const char* password);

    uint32_t allocateUID();

    static uint64_t hashPassword(const char* password);

    bool isRoot(uint32_t uid) const { return uid == ROOT_UID; }

private:
    UserManager() : nextUID(FIRST_USER_UID), userCount(0), initialized(false) {}

    UserManager(const UserManager&)            = delete;
    UserManager& operator=(const UserManager&) = delete;

    void createRootUser();
    bool uidExists(uint32_t uid) const;
    bool nameExists(const char* username) const;
    int  findFreeSlot() const;

    User     users[MAX_USERS];
    uint32_t nextUID;
    uint32_t userCount;
    bool     initialized;
};