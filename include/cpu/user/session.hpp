#pragma once

#include <stdint.h>
#include <stddef.h>

constexpr uint32_t MAX_SESSIONS        = 16;
constexpr uint32_t MAX_PIDS_PER_SESSION = 64;
constexpr uint32_t INVALID_SESSION_ID  = 0xFFFFFFFF;

enum class SessionState {
    Active,
    Inactive,
    Terminated
};

struct Session {
    uint32_t      sessionID;
    uint32_t      uid;
    uint32_t      gid;
    uint32_t      leaderPID;
    uint32_t      pids[MAX_PIDS_PER_SESSION];
    uint32_t      pidCount;
    uint64_t      loginTime;
    uint64_t      lastActivityTime;
    SessionState  state;
    bool          valid;
};

class SessionManager {
public:
    static SessionManager& get();

    void initialize();

    Session* createSession(uint32_t uid, uint32_t gid, uint32_t leaderPID);

    void destroySession(uint32_t sessionID);

    bool suspendSession(uint32_t sessionID);

    bool resumeSession(uint32_t sessionID);

    bool addPIDToSession(uint32_t sessionID, uint32_t pid);

    void removePIDFromSession(uint32_t pid);

    void onProcessExit(uint32_t pid);

    Session* getSessionByID(uint32_t sessionID);

    Session* getSessionByPID(uint32_t pid);

    Session* getActiveSessionForUser(uint32_t uid);

    Session* getSessionByIndex(uint32_t idx);
    uint32_t getSessionCount() const;

    uint32_t allocateSessionID();

    void touchSession(uint32_t pid);

    bool isSessionLeader(uint32_t pid) const;

private:
    SessionManager() : nextSessionID(1), sessionCount(0), initialized(false) {}

    SessionManager(const SessionManager&)            = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    int     findFreeSlot() const;
    int     findSessionSlotByID(uint32_t sessionID) const;
    bool    pidInSession(const Session& s, uint32_t pid) const;
    void    removePIDFromSessionInternal(Session& s, uint32_t pid);

    Session  sessions[MAX_SESSIONS];
    uint32_t nextSessionID;
    uint32_t sessionCount;
    bool     initialized;
};