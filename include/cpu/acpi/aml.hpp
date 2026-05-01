#pragma once

#include <stddef.h>
#include <stdint.h>

namespace AML {

struct NamespaceNode;

enum class ObjectType : uint8_t {
    None,
    Integer,
    String,
    Buffer,
    Package,
    Method,
    Scope,
    Device,
    Processor,
    PowerResource,
    ThermalZone,
    OperationRegion,
    FieldUnit,
    Mutex,
    Event,
    Alias,
    Reference,
};

struct Object {
    ObjectType type = ObjectType::None;
    uint64_t integer = 0;
    const char* string = nullptr;
    const uint8_t* buffer = nullptr;
    size_t length = 0;
    Object* elements = nullptr;
    size_t elementCount = 0;
    NamespaceNode* node = nullptr;
    const uint8_t* methodBody = nullptr;
    size_t methodLength = 0;
    uint8_t methodArgCount = 0;
    uint8_t methodFlags = 0;
};

class Interpreter {
public:
    struct Cursor {
        const uint8_t* ptr = nullptr;
        const uint8_t* end = nullptr;
    };

    Interpreter();

    bool initialize();
    bool loadTable(void* table);

    bool evaluate(const char* path, Object* result);
    bool evaluateInteger(const char* path, uint64_t* result);
    bool getS5SleepTypes(uint16_t* slpTypA, uint16_t* slpTypB);

    size_t namespaceNodeCount() const { return nodeCount; }
    bool isInitialized() const { return initialized; }

private:
    struct ExecutionContext {
        Interpreter* interpreter = nullptr;
        NamespaceNode* scope = nullptr;
        Object args[7];
        Object locals[8];
        bool returned = false;
        bool broke = false;
        Object returnValue;
        uint8_t depth = 0;
    };

    NamespaceNode* root = nullptr;
    bool initialized = false;
    size_t nodeCount = 0;

    NamespaceNode* allocateNode(const char name[4], NamespaceNode* parent);
    NamespaceNode* findChild(NamespaceNode* parent, const char name[4]);
    NamespaceNode* findOrCreateChild(NamespaceNode* parent, const char name[4]);
    NamespaceNode* resolveAsciiPath(const char* path, NamespaceNode* scope, bool create);
    NamespaceNode* resolveNameString(Cursor& cursor, NamespaceNode* scope, bool create);
    NamespaceNode* resolveNameStringAt(const uint8_t* data, const uint8_t* end, NamespaceNode* scope,
                                       bool create, size_t* consumed);

    bool parseTermList(Cursor& cursor, NamespaceNode* scope);
    bool parseNamedObject(Cursor& cursor, NamespaceNode* scope, uint8_t op);
    bool skipTerm(Cursor& cursor);
    bool skipTermArg(Cursor& cursor);
    bool skipNameString(Cursor& cursor);
    bool skipSuperName(Cursor& cursor);
    bool skipFieldList(Cursor& cursor, const uint8_t* fieldEnd, NamespaceNode* scope);

    bool evaluateObject(ExecutionContext& context, Cursor& cursor, Object* result);
    bool executeTermList(ExecutionContext& context, Cursor& cursor, const uint8_t* listEnd);
    bool executeMethod(NamespaceNode* methodNode, const Object* args, size_t argCount, Object* result,
                       uint8_t depth);
    bool evaluateNamedObject(ExecutionContext& context, NamespaceNode* node, Object* result);
    bool evaluateTarget(ExecutionContext& context, Cursor& cursor, Object** targetObject,
                        NamespaceNode** targetNode);
    bool storeToTarget(const Object& value, Object* targetObject, NamespaceNode* targetNode);

    bool makeInteger(uint64_t value, Object* result);
    bool objectToInteger(const Object& object, uint64_t* value);
    bool copyObject(const Object& source, Object* dest);
    Object* allocateObjects(size_t count);
};

}
