#include <cpu/syscall/syscall.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/user/session.hpp>
#include <cpu/user/user.hpp>
#include <interrupts/timer.hpp>
#include <common/string.hpp>

uint64_t Syscall::sys_login(uint64_t login_info_ptr){
    LoginInfo info;
    if(!copyFromUser(&info, login_info_ptr, sizeof(LoginInfo))){
        return (uint64_t)-1;
    }

    char username[32];
    char password[64];
    strncpy(username, info.username, sizeof(username) - 1);
    strncpy(password, info.password, sizeof(password) - 1);
    username[sizeof(username) - 1] = '\0';
    password[sizeof(password) - 1] = '\0';

    if (!UserManager::get().authenticate(username, password)) {
        return (uint64_t)-1;
    }

    uint32_t uid = UserManager::get().getUIDByName(username);
    User* user   = UserManager::get().getUserByUID(uid);
    if (!user) return (uint64_t)-1;

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;

    Session* existing = SessionManager::get().getSessionByPID(current->getPID());
    if (existing) {
        existing->uid = uid;
        existing->gid = user->gid;
        current->setUID(uid);
        current->setGID(user->gid);
        return existing->sessionID;
    }

    Session* session = SessionManager::get().createSession(uid, user->gid, current->getPID());
    if (!session) return (uint64_t)-1;

    session->loginTime        = Timer::get().getTicks();
    session->lastActivityTime = session->loginTime;

    current->setUID(uid);
    current->setGID(user->gid);
    current->setSessionID(session->sessionID);

    return session->sessionID;
}

uint64_t Syscall::sys_logout(uint64_t session_id){
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;

    uint32_t sid = static_cast<uint32_t>(session_id);

    Session* s = SessionManager::get().getSessionByID(sid);
    if (!s) return (uint64_t)-1;
    if (s->uid != current->getUID() && !current->isPrivileged()) return (uint64_t)-1;

    SessionManager::get().destroySession(sid);

    if (current->getSessionID() == sid) {
        current->setUID(INVALID_UID);
        current->setGID(INVALID_GID);
        current->setSessionID(INVALID_SESSION_ID);
    }

    return 0;
}

uint64_t Syscall::sys_getuid(){
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;
    return current->getUID();
}

uint64_t Syscall::sys_getgid(){
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;
    return current->getGID();
}

uint64_t Syscall::sys_setuid(uint64_t uid){
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;

    uint32_t newUID = static_cast<uint32_t>(uid);

    if (!current->isPrivileged() && newUID != current->getUID()) {
        return (uint64_t)-1;
    }

    if (!UserManager::get().getUserByUID(newUID)) {
        return (uint64_t)-1;
    }

    current->setUID(newUID);

    Session* s = SessionManager::get().getSessionByPID(current->getPID());
    if (s && s->leaderPID == current->getPID()) {
        s->uid = newUID;
    }

    return 0;
}

uint64_t Syscall::sys_setgid(uint64_t gid){
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;

    uint32_t newGID = static_cast<uint32_t>(gid);

    if (!current->isPrivileged() && newGID != current->getGID()) {
        return (uint64_t)-1;
    }

    current->setGID(newGID);

    Session* s = SessionManager::get().getSessionByPID(current->getPID());
    if (s && s->leaderPID == current->getPID()) {
        s->gid = newGID;
    }

    return 0;
}

uint64_t Syscall::sys_getsessionid(){
    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;
    return current->getSessionID();
}

uint64_t Syscall::sys_getsessioninfo(uint64_t session_id, uint64_t info_ptr){
    if (!isValidUserPointer(info_ptr, sizeof(SessionInfo))) {
        return (uint64_t)-1;
    }

    Process* current = Scheduler::get().getCurrentProcess();
    if (!current) return (uint64_t)-1;

    uint32_t sid = static_cast<uint32_t>(session_id);
    Session* s   = SessionManager::get().getSessionByID(sid);
    if (!s) return (uint64_t)-1;

    if (s->uid != current->getUID() && !current->isPrivileged()) {
        return (uint64_t)-1;
    }

    SessionInfo out;
    out.sessionID   = s->sessionID;
    out.uid         = s->uid;
    out.gid         = s->gid;
    out.leaderPID   = s->leaderPID;
    out.loginTime   = s->loginTime;
    out.state       = static_cast<uint8_t>(s->state);

    return copyToUser(info_ptr, &out, sizeof(SessionInfo)) ? 0 : (uint64_t)-1;
}

uint64_t Syscall::sys_getuserinfo(uint64_t uid, uint64_t info_ptr) {
    if (!isValidUserPointer(info_ptr, sizeof(UserInfo))) {
        return (uint64_t)-1;
    }
    
    User* user = UserManager::get().getUserByUID(static_cast<uint32_t>(uid));
    if (!user) {
        return (uint64_t)-1;
    }
    
    UserInfo info;
    info.uid = user->uid;
    info.gid = user->gid;
    
    size_t i = 0;
    while (user->username[i] && i < 31) {
        info.username[i] = user->username[i];
        i++;
    }
    info.username[i] = '\0';
    
    i = 0;
    while (user->homeDir[i] && i < 255) {
        info.homeDir[i] = user->homeDir[i];
        i++;
    }
    info.homeDir[i] = '\0';
    
    i = 0;
    while (user->shell[i] && i < 255) {
        info.shell[i] = user->shell[i];
        i++;
    }
    info.shell[i] = '\0';
    
    return copyToUser(info_ptr, &info, sizeof(UserInfo)) ? 0 : (uint64_t)-1;
}
