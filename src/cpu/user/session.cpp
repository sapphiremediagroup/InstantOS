#include <cpu/user/session.hpp>
#include <cpu/user/user.hpp>
#include <common/string.hpp>
#include <cpu/process/scheduler.hpp>

SessionManager& SessionManager::get() {
    static SessionManager sessionManagerInstance;
    return sessionManagerInstance;
}

void SessionManager::initialize() {
    if (initialized) return;

    for (uint32_t i = 0; i < MAX_SESSIONS; i++) {
        sessions[i].valid    = false;
        sessions[i].pidCount = 0;
        sessions[i].state    = SessionState::Terminated;
    }

    nextSessionID = 1;
    sessionCount  = 0;
    initialized   = true;
}

int SessionManager::findFreeSlot() const {
    for (uint32_t i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].valid) return static_cast<int>(i);
    }
    return -1;
}

int SessionManager::findSessionSlotByID(uint32_t sessionID) const {
    for (uint32_t i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].valid && sessions[i].sessionID == sessionID)
            return static_cast<int>(i);
    }
    return -1;
}

bool SessionManager::pidInSession(const Session& s, uint32_t pid) const {
    for (uint32_t i = 0; i < s.pidCount; i++) {
        if (s.pids[i] == pid) return true;
    }
    return false;
}

void SessionManager::removePIDFromSessionInternal(Session& s, uint32_t pid) {
    for (uint32_t i = 0; i < s.pidCount; i++) {
        if (s.pids[i] == pid) {
            for (uint32_t j = i; j + 1 < s.pidCount; j++) {
                s.pids[j] = s.pids[j + 1];
            }
            s.pidCount--;
            return;
        }
    }
}

Session* SessionManager::createSession(uint32_t uid, uint32_t gid, uint32_t leaderPID) {
    if (!initialized) return nullptr;

    if (getSessionByPID(leaderPID) != nullptr) return nullptr;

    int slot = findFreeSlot();
    if (slot < 0) return nullptr;

    Session& s           = sessions[slot];
    s.sessionID          = allocateSessionID();
    s.uid                = uid;
    s.gid                = gid;
    s.leaderPID          = leaderPID;
    s.pidCount           = 0;
    s.loginTime          = 0;
    s.lastActivityTime   = 0;
    s.state              = SessionState::Active;
    s.valid              = true;

    s.pids[s.pidCount++] = leaderPID;

    sessionCount++;
    return &s;
}

void SessionManager::destroySession(uint32_t sessionID) {
    if (!initialized) return;

    int slot = findSessionSlotByID(sessionID);
    if (slot < 0) return;

    Session& s = sessions[slot];

    for (uint32_t i = 0; i < s.pidCount; i++) {
        uint32_t pid = s.pids[i];
        if (pid == s.leaderPID) continue;

        Process* proc = Scheduler::get().getProcessByPID(pid);
        if (proc) proc->sendSignal(SIGTERM);
    }

    Process* leader = Scheduler::get().getProcessByPID(s.leaderPID);
    if (leader) leader->sendSignal(SIGTERM);

    s.state = SessionState::Terminated;
    s.valid = false;
    sessionCount--;
}

bool SessionManager::suspendSession(uint32_t sessionID) {
    int slot = findSessionSlotByID(sessionID);
    if (slot < 0) return false;

    Session& s = sessions[slot];
    if (s.state != SessionState::Active) return false;

    s.state = SessionState::Inactive;
    return true;
}

bool SessionManager::resumeSession(uint32_t sessionID) {
    int slot = findSessionSlotByID(sessionID);
    if (slot < 0) return false;

    Session& s = sessions[slot];
    if (s.state != SessionState::Inactive) return false;

    s.state = SessionState::Active;
    return true;
}

bool SessionManager::addPIDToSession(uint32_t sessionID, uint32_t pid) {
    if (!initialized) return false;

    int slot = findSessionSlotByID(sessionID);
    if (slot < 0) return false;

    Session& s = sessions[slot];
    if (s.pidCount >= MAX_PIDS_PER_SESSION) return false;
    if (pidInSession(s, pid))              return false;

    s.pids[s.pidCount++] = pid;
    return true;
}

void SessionManager::removePIDFromSession(uint32_t pid) {
    if (!initialized) return;

    for (uint32_t i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].valid) continue;

        Session& s = sessions[i];
        if (!pidInSession(s, pid)) continue;

        removePIDFromSessionInternal(s, pid);

        if (pid == s.leaderPID) {
            if (s.pidCount == 0) {
                s.state = SessionState::Terminated;
                s.valid = false;
                sessionCount--;
            } else {
                s.leaderPID = s.pids[0];
            }
        } else if (s.pidCount == 0) {
            s.state = SessionState::Terminated;
            s.valid = false;
            sessionCount--;
        }

        return;
    }
}

void SessionManager::onProcessExit(uint32_t pid) {
    removePIDFromSession(pid);
}

Session* SessionManager::getSessionByID(uint32_t sessionID) {
    int slot = findSessionSlotByID(sessionID);
    return slot >= 0 ? &sessions[slot] : nullptr;
}

Session* SessionManager::getSessionByPID(uint32_t pid) {
    for (uint32_t i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].valid) continue;
        if (pidInSession(sessions[i], pid)) return &sessions[i];
    }
    return nullptr;
}

Session* SessionManager::getActiveSessionForUser(uint32_t uid) {
    for (uint32_t i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].valid &&
            sessions[i].uid   == uid &&
            sessions[i].state == SessionState::Active) {
            return &sessions[i];
        }
    }
    return nullptr;
}

Session* SessionManager::getSessionByIndex(uint32_t idx) {
    if (idx >= MAX_SESSIONS) return nullptr;
    return sessions[idx].valid ? &sessions[idx] : nullptr;
}

uint32_t SessionManager::getSessionCount() const {
    return sessionCount;
}

uint32_t SessionManager::allocateSessionID() {
    uint32_t id = nextSessionID++;
    if (nextSessionID == INVALID_SESSION_ID) nextSessionID = 1;
    return id;
}

void SessionManager::touchSession(uint32_t pid) {
    Session* s = getSessionByPID(pid);
    if (s) {
        s->lastActivityTime++;
    }
}

bool SessionManager::isSessionLeader(uint32_t pid) const {
    for (uint32_t i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].valid && sessions[i].leaderPID == pid) return true;
    }
    return false;
}