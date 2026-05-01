#include <cpu/acpi/aml.hpp>

#include <common/string.hpp>
#include <memory/heap.hpp>

namespace AML {

namespace {

constexpr uint8_t AML_ZERO_OP = 0x00;
constexpr uint8_t AML_ONE_OP = 0x01;
constexpr uint8_t AML_ALIAS_OP = 0x06;
constexpr uint8_t AML_NAME_OP = 0x08;
constexpr uint8_t AML_BYTE_PREFIX = 0x0A;
constexpr uint8_t AML_WORD_PREFIX = 0x0B;
constexpr uint8_t AML_DWORD_PREFIX = 0x0C;
constexpr uint8_t AML_STRING_PREFIX = 0x0D;
constexpr uint8_t AML_QWORD_PREFIX = 0x0E;
constexpr uint8_t AML_SCOPE_OP = 0x10;
constexpr uint8_t AML_BUFFER_OP = 0x11;
constexpr uint8_t AML_PACKAGE_OP = 0x12;
constexpr uint8_t AML_VAR_PACKAGE_OP = 0x13;
constexpr uint8_t AML_METHOD_OP = 0x14;
constexpr uint8_t AML_EXTERNAL_OP = 0x15;
constexpr uint8_t AML_DUAL_NAME_PREFIX = 0x2E;
constexpr uint8_t AML_MULTI_NAME_PREFIX = 0x2F;
constexpr uint8_t AML_ROOT_CHAR = 0x5C;
constexpr uint8_t AML_PARENT_PREFIX_CHAR = 0x5E;
constexpr uint8_t AML_LOCAL0 = 0x60;
constexpr uint8_t AML_ARG0 = 0x68;
constexpr uint8_t AML_STORE_OP = 0x70;
constexpr uint8_t AML_REF_OF_OP = 0x71;
constexpr uint8_t AML_ADD_OP = 0x72;
constexpr uint8_t AML_SUBTRACT_OP = 0x74;
constexpr uint8_t AML_INCREMENT_OP = 0x75;
constexpr uint8_t AML_DECREMENT_OP = 0x76;
constexpr uint8_t AML_MULTIPLY_OP = 0x77;
constexpr uint8_t AML_DIVIDE_OP = 0x78;
constexpr uint8_t AML_SHIFT_LEFT_OP = 0x79;
constexpr uint8_t AML_SHIFT_RIGHT_OP = 0x7A;
constexpr uint8_t AML_AND_OP = 0x7B;
constexpr uint8_t AML_NAND_OP = 0x7C;
constexpr uint8_t AML_OR_OP = 0x7D;
constexpr uint8_t AML_NOR_OP = 0x7E;
constexpr uint8_t AML_XOR_OP = 0x7F;
constexpr uint8_t AML_NOT_OP = 0x80;
constexpr uint8_t AML_DEREF_OF_OP = 0x83;
constexpr uint8_t AML_MOD_OP = 0x85;
constexpr uint8_t AML_NOTIFY_OP = 0x86;
constexpr uint8_t AML_SIZE_OF_OP = 0x87;
constexpr uint8_t AML_INDEX_OP = 0x88;
constexpr uint8_t AML_OBJECT_TYPE_OP = 0x8E;
constexpr uint8_t AML_LAND_OP = 0x90;
constexpr uint8_t AML_LOR_OP = 0x91;
constexpr uint8_t AML_LNOT_OP = 0x92;
constexpr uint8_t AML_LEQUAL_OP = 0x93;
constexpr uint8_t AML_LGREATER_OP = 0x94;
constexpr uint8_t AML_LLESS_OP = 0x95;
constexpr uint8_t AML_TO_INTEGER_OP = 0x99;
constexpr uint8_t AML_CONTINUE_OP = 0x9F;
constexpr uint8_t AML_IF_OP = 0xA0;
constexpr uint8_t AML_ELSE_OP = 0xA1;
constexpr uint8_t AML_WHILE_OP = 0xA2;
constexpr uint8_t AML_NOOP_OP = 0xA3;
constexpr uint8_t AML_RETURN_OP = 0xA4;
constexpr uint8_t AML_BREAK_OP = 0xA5;
constexpr uint8_t AML_ONES_OP = 0xFF;

constexpr uint8_t AML_EXT_OP = 0x5B;
constexpr uint8_t AML_EXT_MUTEX_OP = 0x01;
constexpr uint8_t AML_EXT_EVENT_OP = 0x02;
constexpr uint8_t AML_EXT_COND_REF_OF_OP = 0x12;
constexpr uint8_t AML_EXT_STALL_OP = 0x21;
constexpr uint8_t AML_EXT_SLEEP_OP = 0x22;
constexpr uint8_t AML_EXT_ACQUIRE_OP = 0x23;
constexpr uint8_t AML_EXT_RELEASE_OP = 0x27;
constexpr uint8_t AML_EXT_REVISION_OP = 0x30;
constexpr uint8_t AML_EXT_DEBUG_OP = 0x31;
constexpr uint8_t AML_EXT_TIMER_OP = 0x33;
constexpr uint8_t AML_EXT_REGION_OP = 0x80;
constexpr uint8_t AML_EXT_FIELD_OP = 0x81;
constexpr uint8_t AML_EXT_DEVICE_OP = 0x82;
constexpr uint8_t AML_EXT_PROCESSOR_OP = 0x83;
constexpr uint8_t AML_EXT_POWER_RES_OP = 0x84;
constexpr uint8_t AML_EXT_THERMAL_ZONE_OP = 0x85;
constexpr uint8_t AML_EXT_INDEX_FIELD_OP = 0x86;
constexpr uint8_t AML_EXT_BANK_FIELD_OP = 0x87;

constexpr uint8_t AML_FIELD_RESERVED_OP = 0x00;
constexpr uint8_t AML_FIELD_ACCESS_OP = 0x01;
constexpr uint8_t AML_FIELD_CONNECTION_OP = 0x02;
constexpr uint8_t AML_FIELD_EXT_ACCESS_OP = 0x03;

constexpr uint8_t kMaxMethodDepth = 16;
constexpr uint32_t kMaxWhileIterations = 1024;

struct AcpiHeaderView {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oemId[6];
    char oemTableId[8];
    uint32_t oemRevision;
    uint32_t creatorId;
    uint32_t creatorRevision;
} __attribute__((packed));

uint16_t readLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(data[1] << 8);
}

uint32_t readLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t readLe64(const uint8_t* data) {
    return static_cast<uint64_t>(readLe32(data)) |
           (static_cast<uint64_t>(readLe32(data + 4)) << 32);
}

bool readPkgLengthAt(const uint8_t* data, const uint8_t* end, uint32_t* value, size_t* consumed) {
    if (!data || data >= end || !value || !consumed) {
        return false;
    }

    const uint8_t lead = *data;
    const uint8_t followBytes = lead >> 6;
    if (data + 1 + followBytes > end) {
        return false;
    }

    uint32_t result = lead & 0x3F;
    if (followBytes != 0) {
        result = lead & 0x0F;
        for (uint8_t i = 0; i < followBytes; ++i) {
            result |= static_cast<uint32_t>(data[1 + i]) << (4 + i * 8);
        }
    }

    *value = result;
    *consumed = 1 + followBytes;
    return true;
}

bool readPkgLength(Interpreter::Cursor& cursor, uint32_t* value, const uint8_t** packageStart) {
    size_t consumed = 0;
    const uint8_t* start = cursor.ptr;
    if (!readPkgLengthAt(cursor.ptr, cursor.end, value, &consumed)) {
        return false;
    }

    cursor.ptr += consumed;
    if (packageStart) {
        *packageStart = start;
    }
    return true;
}

bool isNameLead(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || c == '_';
}

bool isNameChar(uint8_t c) {
    return isNameLead(c) || (c >= '0' && c <= '9');
}

void padNameSegment(char out[4], const char* input, size_t length) {
    for (size_t i = 0; i < 4; ++i) {
        out[i] = '_';
    }
    for (size_t i = 0; i < length && i < 4; ++i) {
        out[i] = input[i];
    }
}

bool sameSegment(const char a[4], const char b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

bool isPackageOpcode(uint8_t op) {
    if (op == AML_SCOPE_OP || op == AML_BUFFER_OP || op == AML_PACKAGE_OP ||
        op == AML_VAR_PACKAGE_OP || op == AML_METHOD_OP || op == AML_IF_OP ||
        op == AML_ELSE_OP || op == AML_WHILE_OP) {
        return true;
    }
    return false;
}

uint8_t acpiObjectTypeValue(ObjectType type) {
    switch (type) {
        case ObjectType::Integer: return 1;
        case ObjectType::String: return 2;
        case ObjectType::Buffer: return 3;
        case ObjectType::Package: return 4;
        case ObjectType::FieldUnit: return 5;
        case ObjectType::Device: return 6;
        case ObjectType::Event: return 7;
        case ObjectType::Method: return 8;
        case ObjectType::Mutex: return 9;
        case ObjectType::OperationRegion: return 10;
        case ObjectType::PowerResource: return 11;
        case ObjectType::Processor: return 12;
        case ObjectType::ThermalZone: return 13;
        default: return 0;
    }
}

}

struct NamespaceNode {
    char name[5];
    NamespaceNode* parent = nullptr;
    NamespaceNode* child = nullptr;
    NamespaceNode* next = nullptr;
    Object object;
    bool defined = false;
};

Interpreter::Interpreter() = default;

bool Interpreter::initialize() {
    if (initialized) {
        return true;
    }

    char rootName[4] = { '\\', '_', '_', '_' };
    root = allocateNode(rootName, nullptr);
    if (!root) {
        return false;
    }

    root->object.type = ObjectType::Scope;
    root->defined = true;

    char revName[4] = { '_', 'R', 'E', 'V' };
    NamespaceNode* rev = findOrCreateChild(root, revName);
    if (rev) {
        rev->object.type = ObjectType::Integer;
        rev->object.integer = 2;
        rev->defined = true;
    }

    static const char osNameString[] = "InstantOS";
    char osName[4] = { '_', 'O', 'S', '_' };
    NamespaceNode* os = findOrCreateChild(root, osName);
    if (os) {
        os->object.type = ObjectType::String;
        os->object.string = osNameString;
        os->object.length = sizeof(osNameString) - 1;
        os->defined = true;
    }

    char osiName[4] = { '_', 'O', 'S', 'I' };
    NamespaceNode* osi = findOrCreateChild(root, osiName);
    if (osi) {
        osi->object.type = ObjectType::Method;
        osi->object.methodArgCount = 1;
        osi->object.methodBody = nullptr;
        osi->object.methodLength = 0;
        osi->defined = true;
    }

    initialized = true;
    return true;
}

bool Interpreter::loadTable(void* table) {
    if (!table && !initialize()) {
        return false;
    }
    if (!table) {
        return false;
    }
    if (!initialized && !initialize()) {
        return false;
    }

    AcpiHeaderView* header = static_cast<AcpiHeaderView*>(table);
    if (header->length <= sizeof(AcpiHeaderView)) {
        return false;
    }

    Cursor cursor;
    cursor.ptr = reinterpret_cast<const uint8_t*>(header) + sizeof(AcpiHeaderView);
    cursor.end = reinterpret_cast<const uint8_t*>(header) + header->length;
    return parseTermList(cursor, root);
}

NamespaceNode* Interpreter::allocateNode(const char name[4], NamespaceNode* parent) {
    NamespaceNode* node = reinterpret_cast<NamespaceNode*>(kmalloc(sizeof(NamespaceNode)));
    if (!node) {
        return nullptr;
    }

    memset(node, 0, sizeof(NamespaceNode));
    for (size_t i = 0; i < 4; ++i) {
        node->name[i] = name[i];
    }
    node->name[4] = 0;
    node->parent = parent;
    node->defined = true;
    if (parent) {
        node->next = parent->child;
        parent->child = node;
    }
    ++nodeCount;
    return node;
}

NamespaceNode* Interpreter::findChild(NamespaceNode* parent, const char name[4]) {
    if (!parent) {
        return nullptr;
    }

    for (NamespaceNode* child = parent->child; child; child = child->next) {
        if (sameSegment(child->name, name)) {
            return child;
        }
    }
    return nullptr;
}

NamespaceNode* Interpreter::findOrCreateChild(NamespaceNode* parent, const char name[4]) {
    NamespaceNode* child = findChild(parent, name);
    if (child) {
        return child;
    }
    return allocateNode(name, parent);
}

NamespaceNode* Interpreter::resolveAsciiPath(const char* path, NamespaceNode* scope, bool create) {
    if (!path || !root) {
        return nullptr;
    }

    NamespaceNode* current = scope ? scope : root;
    bool absolute = false;
    if (*path == '\\' || *path == '/') {
        absolute = true;
        current = root;
        ++path;
    }

    while (*path == '^') {
        if (current->parent) {
            current = current->parent;
        }
        ++path;
    }

    if (*path == 0) {
        return current;
    }

    char segment[4];
    bool firstSegment = true;
    while (*path) {
        while (*path == '.' || *path == '\\' || *path == '/') {
            ++path;
        }
        if (*path == 0) {
            break;
        }

        char text[4];
        size_t length = 0;
        while (*path && *path != '.' && *path != '\\' && *path != '/' && length < 4) {
            text[length++] = *path++;
        }
        while (*path && *path != '.' && *path != '\\' && *path != '/') {
            ++path;
        }
        padNameSegment(segment, text, length);

        NamespaceNode* next = nullptr;
        if (!create && !absolute && firstSegment) {
            for (NamespaceNode* search = current; search && !next; search = search->parent) {
                next = findChild(search, segment);
            }
        } else {
            next = create ? findOrCreateChild(current, segment) : findChild(current, segment);
        }

        if (!next) {
            return nullptr;
        }
        current = next;
        firstSegment = false;
    }

    return current;
}

NamespaceNode* Interpreter::resolveNameStringAt(const uint8_t* data, const uint8_t* end, NamespaceNode* scope,
                                                bool create, size_t* consumed) {
    Cursor cursor;
    cursor.ptr = data;
    cursor.end = end;
    NamespaceNode* node = resolveNameString(cursor, scope, create);
    if (consumed) {
        *consumed = static_cast<size_t>(cursor.ptr - data);
    }
    return node;
}

NamespaceNode* Interpreter::resolveNameString(Cursor& cursor, NamespaceNode* scope, bool create) {
    if (!root || cursor.ptr >= cursor.end) {
        return nullptr;
    }

    NamespaceNode* current = scope ? scope : root;
    bool absolute = false;
    uint8_t parentPrefixes = 0;

    if (*cursor.ptr == AML_ROOT_CHAR) {
        absolute = true;
        current = root;
        ++cursor.ptr;
    }

    while (cursor.ptr < cursor.end && *cursor.ptr == AML_PARENT_PREFIX_CHAR) {
        if (current->parent) {
            current = current->parent;
        }
        ++cursor.ptr;
        ++parentPrefixes;
    }

    if (cursor.ptr >= cursor.end) {
        return current;
    }

    if (*cursor.ptr == 0x00) {
        ++cursor.ptr;
        return current;
    }

    uint8_t segmentCount = 1;
    if (*cursor.ptr == AML_DUAL_NAME_PREFIX) {
        segmentCount = 2;
        ++cursor.ptr;
    } else if (*cursor.ptr == AML_MULTI_NAME_PREFIX) {
        ++cursor.ptr;
        if (cursor.ptr >= cursor.end) {
            return nullptr;
        }
        segmentCount = *cursor.ptr++;
        if (segmentCount == 0) {
            return current;
        }
    }

    if (cursor.ptr + static_cast<size_t>(segmentCount) * 4 > cursor.end) {
        return nullptr;
    }

    const bool searchFirstSegment = !create && !absolute && parentPrefixes == 0 && segmentCount == 1;
    for (uint8_t segmentIndex = 0; segmentIndex < segmentCount; ++segmentIndex) {
        char segment[4];
        for (uint8_t i = 0; i < 4; ++i) {
            segment[i] = static_cast<char>(*cursor.ptr++);
        }

        NamespaceNode* next = nullptr;
        if (searchFirstSegment && segmentIndex == 0) {
            for (NamespaceNode* search = current; search && !next; search = search->parent) {
                next = findChild(search, segment);
            }
        } else {
            next = create ? findOrCreateChild(current, segment) : findChild(current, segment);
        }

        if (!next) {
            return nullptr;
        }
        current = next;
    }

    return current;
}

bool Interpreter::parseTermList(Cursor& cursor, NamespaceNode* scope) {
    while (cursor.ptr < cursor.end) {
        const uint8_t* before = cursor.ptr;
        uint8_t op = *cursor.ptr++;
        if (parseNamedObject(cursor, scope, op)) {
            continue;
        }

        cursor.ptr = before;
        if (!skipTerm(cursor) || cursor.ptr <= before) {
            cursor.ptr = before + 1;
        }
    }
    return true;
}

bool Interpreter::parseNamedObject(Cursor& cursor, NamespaceNode* scope, uint8_t op) {
    if (op == AML_NAME_OP) {
        NamespaceNode* node = resolveNameString(cursor, scope, true);
        if (!node) {
            return false;
        }

        Cursor valueCursor = cursor;
        ExecutionContext context;
        context.interpreter = this;
        context.scope = scope;
        Object value;
        if (evaluateObject(context, valueCursor, &value)) {
            node->object = value;
            node->defined = true;
            cursor = valueCursor;
        } else {
            skipTermArg(cursor);
            node->object.type = ObjectType::None;
            node->defined = true;
        }
        return true;
    }

    if (op == AML_ALIAS_OP) {
        NamespaceNode* source = resolveNameString(cursor, scope, false);
        NamespaceNode* alias = resolveNameString(cursor, scope, true);
        if (alias) {
            alias->object.type = ObjectType::Alias;
            alias->object.node = source;
            alias->defined = true;
        }
        return true;
    }

    if (op == AML_SCOPE_OP || op == AML_METHOD_OP) {
        const uint8_t* packageStart = nullptr;
        uint32_t packageLength = 0;
        if (!readPkgLength(cursor, &packageLength, &packageStart)) {
            return false;
        }
        const uint8_t* packageEnd = packageStart + packageLength;
        if (packageEnd > cursor.end || packageEnd < cursor.ptr) {
            return false;
        }

        NamespaceNode* node = resolveNameString(cursor, scope, true);
        if (!node) {
            cursor.ptr = packageEnd;
            return true;
        }

        if (op == AML_SCOPE_OP) {
            node->object.type = ObjectType::Scope;
            Cursor body;
            body.ptr = cursor.ptr;
            body.end = packageEnd;
            parseTermList(body, node);
        } else {
            if (cursor.ptr >= packageEnd) {
                cursor.ptr = packageEnd;
                return true;
            }
            const uint8_t flags = *cursor.ptr++;
            node->object.type = ObjectType::Method;
            node->object.methodFlags = flags;
            node->object.methodArgCount = flags & 0x07;
            node->object.methodBody = cursor.ptr;
            node->object.methodLength = static_cast<size_t>(packageEnd - cursor.ptr);
        }
        node->defined = true;
        cursor.ptr = packageEnd;
        return true;
    }

    if (op == AML_EXTERNAL_OP) {
        NamespaceNode* node = resolveNameString(cursor, scope, true);
        if (node && cursor.ptr + 2 <= cursor.end) {
            ++cursor.ptr;
            node->object.methodArgCount = *cursor.ptr++;
            node->defined = true;
        }
        return true;
    }

    if (op != AML_EXT_OP || cursor.ptr >= cursor.end) {
        return false;
    }

    const uint8_t ext = *cursor.ptr++;
    if (ext == AML_EXT_DEVICE_OP || ext == AML_EXT_THERMAL_ZONE_OP ||
        ext == AML_EXT_PROCESSOR_OP || ext == AML_EXT_POWER_RES_OP) {
        const uint8_t* packageStart = nullptr;
        uint32_t packageLength = 0;
        if (!readPkgLength(cursor, &packageLength, &packageStart)) {
            return false;
        }
        const uint8_t* packageEnd = packageStart + packageLength;
        if (packageEnd > cursor.end || packageEnd < cursor.ptr) {
            return false;
        }

        NamespaceNode* node = resolveNameString(cursor, scope, true);
        if (!node) {
            cursor.ptr = packageEnd;
            return true;
        }

        if (ext == AML_EXT_DEVICE_OP) {
            node->object.type = ObjectType::Device;
        } else if (ext == AML_EXT_THERMAL_ZONE_OP) {
            node->object.type = ObjectType::ThermalZone;
        } else if (ext == AML_EXT_PROCESSOR_OP) {
            node->object.type = ObjectType::Processor;
            if (cursor.ptr + 6 <= packageEnd) {
                cursor.ptr += 6;
            }
        } else {
            node->object.type = ObjectType::PowerResource;
            if (cursor.ptr + 3 <= packageEnd) {
                cursor.ptr += 3;
            }
        }

        Cursor body;
        body.ptr = cursor.ptr;
        body.end = packageEnd;
        parseTermList(body, node);
        node->defined = true;
        cursor.ptr = packageEnd;
        return true;
    }

    if (ext == AML_EXT_REGION_OP) {
        NamespaceNode* node = resolveNameString(cursor, scope, true);
        if (!node || cursor.ptr >= cursor.end) {
            return true;
        }
        ++cursor.ptr;
        skipTermArg(cursor);
        skipTermArg(cursor);
        node->object.type = ObjectType::OperationRegion;
        node->defined = true;
        return true;
    }

    if (ext == AML_EXT_FIELD_OP || ext == AML_EXT_INDEX_FIELD_OP || ext == AML_EXT_BANK_FIELD_OP) {
        const uint8_t* packageStart = nullptr;
        uint32_t packageLength = 0;
        if (!readPkgLength(cursor, &packageLength, &packageStart)) {
            return false;
        }
        const uint8_t* packageEnd = packageStart + packageLength;
        if (packageEnd > cursor.end || packageEnd < cursor.ptr) {
            return false;
        }

        skipNameString(cursor);
        if (ext == AML_EXT_INDEX_FIELD_OP) {
            skipNameString(cursor);
        } else if (ext == AML_EXT_BANK_FIELD_OP) {
            skipNameString(cursor);
            skipTermArg(cursor);
        }
        if (cursor.ptr < packageEnd) {
            ++cursor.ptr;
        }
        skipFieldList(cursor, packageEnd, scope);
        cursor.ptr = packageEnd;
        return true;
    }

    if (ext == AML_EXT_MUTEX_OP || ext == AML_EXT_EVENT_OP) {
        NamespaceNode* node = resolveNameString(cursor, scope, true);
        if (node) {
            node->object.type = (ext == AML_EXT_MUTEX_OP) ? ObjectType::Mutex : ObjectType::Event;
            node->defined = true;
        }
        if (ext == AML_EXT_MUTEX_OP && cursor.ptr < cursor.end) {
            ++cursor.ptr;
        }
        return true;
    }

    return false;
}

bool Interpreter::skipFieldList(Cursor& cursor, const uint8_t* fieldEnd, NamespaceNode* scope) {
    while (cursor.ptr < fieldEnd) {
        uint8_t op = *cursor.ptr;
        if (op == AML_FIELD_RESERVED_OP) {
            ++cursor.ptr;
            uint32_t bitLength = 0;
            const uint8_t* ignored = nullptr;
            readPkgLength(cursor, &bitLength, &ignored);
            continue;
        }
        if (op == AML_FIELD_ACCESS_OP) {
            cursor.ptr += (cursor.ptr + 3 <= fieldEnd) ? 3 : static_cast<size_t>(fieldEnd - cursor.ptr);
            continue;
        }
        if (op == AML_FIELD_CONNECTION_OP) {
            ++cursor.ptr;
            if (cursor.ptr < fieldEnd && *cursor.ptr == AML_BUFFER_OP) {
                skipTermArg(cursor);
            } else {
                skipNameString(cursor);
            }
            continue;
        }
        if (op == AML_FIELD_EXT_ACCESS_OP) {
            cursor.ptr += (cursor.ptr + 4 <= fieldEnd) ? 4 : static_cast<size_t>(fieldEnd - cursor.ptr);
            continue;
        }
        if (cursor.ptr + 4 > fieldEnd) {
            cursor.ptr = fieldEnd;
            return true;
        }

        char name[4];
        for (uint8_t i = 0; i < 4; ++i) {
            name[i] = static_cast<char>(*cursor.ptr++);
        }
        uint32_t bitLength = 0;
        const uint8_t* ignored = nullptr;
        readPkgLength(cursor, &bitLength, &ignored);
        NamespaceNode* field = findOrCreateChild(scope, name);
        if (field) {
            field->object.type = ObjectType::FieldUnit;
            field->object.integer = bitLength;
            field->defined = true;
        }
    }
    return true;
}

bool Interpreter::skipNameString(Cursor& cursor) {
    Cursor temp = cursor;
    NamespaceNode* node = resolveNameString(temp, temp.ptr < temp.end ? root : nullptr, false);
    if (!node && temp.ptr == cursor.ptr) {
        if (cursor.ptr < cursor.end && (isNameLead(*cursor.ptr) || *cursor.ptr == AML_ROOT_CHAR ||
                                       *cursor.ptr == AML_PARENT_PREFIX_CHAR ||
                                       *cursor.ptr == AML_DUAL_NAME_PREFIX ||
                                       *cursor.ptr == AML_MULTI_NAME_PREFIX || *cursor.ptr == 0x00)) {
            size_t consumed = 0;
            resolveNameStringAt(cursor.ptr, cursor.end, root, true, &consumed);
            cursor.ptr += consumed;
            return consumed != 0;
        }
        return false;
    }
    cursor = temp;
    return true;
}

bool Interpreter::skipSuperName(Cursor& cursor) {
    if (cursor.ptr >= cursor.end) {
        return false;
    }

    uint8_t op = *cursor.ptr;
    if (op == AML_ZERO_OP || (op >= AML_LOCAL0 && op <= AML_LOCAL0 + 7) ||
        (op >= AML_ARG0 && op <= AML_ARG0 + 6) || op == AML_EXT_DEBUG_OP) {
        ++cursor.ptr;
        return true;
    }
    if (op == AML_INDEX_OP) {
        ++cursor.ptr;
        skipTermArg(cursor);
        skipTermArg(cursor);
        skipSuperName(cursor);
        return true;
    }
    return skipNameString(cursor);
}

bool Interpreter::skipTermArg(Cursor& cursor) {
    if (cursor.ptr >= cursor.end) {
        return false;
    }

    const uint8_t* before = cursor.ptr;
    uint8_t op = *cursor.ptr++;
    switch (op) {
        case AML_ZERO_OP:
        case AML_ONE_OP:
        case AML_ONES_OP:
        case AML_NOOP_OP:
            return true;
        case AML_BYTE_PREFIX:
            if (cursor.ptr + 1 <= cursor.end) cursor.ptr += 1;
            return true;
        case AML_WORD_PREFIX:
            if (cursor.ptr + 2 <= cursor.end) cursor.ptr += 2;
            return true;
        case AML_DWORD_PREFIX:
            if (cursor.ptr + 4 <= cursor.end) cursor.ptr += 4;
            return true;
        case AML_QWORD_PREFIX:
            if (cursor.ptr + 8 <= cursor.end) cursor.ptr += 8;
            return true;
        case AML_STRING_PREFIX:
            while (cursor.ptr < cursor.end && *cursor.ptr != 0) {
                ++cursor.ptr;
            }
            if (cursor.ptr < cursor.end) {
                ++cursor.ptr;
            }
            return true;
        case AML_BUFFER_OP:
        case AML_PACKAGE_OP:
        case AML_VAR_PACKAGE_OP:
        case AML_SCOPE_OP:
        case AML_METHOD_OP:
        case AML_IF_OP:
        case AML_ELSE_OP:
        case AML_WHILE_OP: {
            cursor.ptr = before + 1;
            const uint8_t* packageStart = nullptr;
            uint32_t packageLength = 0;
            if (!readPkgLength(cursor, &packageLength, &packageStart)) {
                return false;
            }
            const uint8_t* packageEnd = packageStart + packageLength;
            cursor.ptr = packageEnd <= cursor.end ? packageEnd : cursor.end;
            return true;
        }
        case AML_RETURN_OP:
        case AML_REF_OF_OP:
        case AML_DEREF_OF_OP:
        case AML_SIZE_OF_OP:
        case AML_LNOT_OP:
        case AML_TO_INTEGER_OP:
            return skipTermArg(cursor);
        case AML_STORE_OP:
            skipTermArg(cursor);
            return skipSuperName(cursor);
        case AML_INCREMENT_OP:
        case AML_DECREMENT_OP:
            return skipSuperName(cursor);
        case AML_ADD_OP:
        case AML_SUBTRACT_OP:
        case AML_MULTIPLY_OP:
        case AML_SHIFT_LEFT_OP:
        case AML_SHIFT_RIGHT_OP:
        case AML_AND_OP:
        case AML_NAND_OP:
        case AML_OR_OP:
        case AML_NOR_OP:
        case AML_XOR_OP:
        case AML_MOD_OP:
            skipTermArg(cursor);
            skipTermArg(cursor);
            return skipSuperName(cursor);
        case AML_DIVIDE_OP:
            skipTermArg(cursor);
            skipTermArg(cursor);
            skipSuperName(cursor);
            return skipSuperName(cursor);
        case AML_LAND_OP:
        case AML_LOR_OP:
        case AML_LEQUAL_OP:
        case AML_LGREATER_OP:
        case AML_LLESS_OP:
            skipTermArg(cursor);
            return skipTermArg(cursor);
        case AML_INDEX_OP:
            skipTermArg(cursor);
            skipTermArg(cursor);
            return skipSuperName(cursor);
        case AML_NOTIFY_OP:
            skipSuperName(cursor);
            return skipTermArg(cursor);
        case AML_OBJECT_TYPE_OP:
            return skipSuperName(cursor);
        case AML_EXT_OP:
            if (cursor.ptr >= cursor.end) {
                return false;
            }
            if (isPackageOpcode(*cursor.ptr)) {
                return skipTermArg(cursor);
            }
            switch (*cursor.ptr++) {
                case AML_EXT_REVISION_OP:
                case AML_EXT_DEBUG_OP:
                case AML_EXT_TIMER_OP:
                    return true;
                case AML_EXT_COND_REF_OF_OP:
                    skipSuperName(cursor);
                    return skipSuperName(cursor);
                case AML_EXT_STALL_OP:
                case AML_EXT_SLEEP_OP:
                    return skipTermArg(cursor);
                case AML_EXT_ACQUIRE_OP:
                    skipSuperName(cursor);
                    return skipTermArg(cursor);
                case AML_EXT_RELEASE_OP:
                    return skipSuperName(cursor);
                case AML_EXT_DEVICE_OP:
                case AML_EXT_PROCESSOR_OP:
                case AML_EXT_POWER_RES_OP:
                case AML_EXT_THERMAL_ZONE_OP:
                case AML_EXT_FIELD_OP:
                case AML_EXT_INDEX_FIELD_OP:
                case AML_EXT_BANK_FIELD_OP: {
                    const uint8_t* packageStart = nullptr;
                    uint32_t packageLength = 0;
                    if (!readPkgLength(cursor, &packageLength, &packageStart)) {
                        return false;
                    }
                    const uint8_t* packageEnd = packageStart + packageLength;
                    cursor.ptr = packageEnd <= cursor.end ? packageEnd : cursor.end;
                    return true;
                }
                case AML_EXT_REGION_OP:
                    skipNameString(cursor);
                    if (cursor.ptr < cursor.end) ++cursor.ptr;
                    skipTermArg(cursor);
                    return skipTermArg(cursor);
                default:
                    return true;
            }
        default:
            cursor.ptr = before;
            if (isNameLead(*cursor.ptr) || *cursor.ptr == AML_ROOT_CHAR ||
                *cursor.ptr == AML_PARENT_PREFIX_CHAR || *cursor.ptr == AML_DUAL_NAME_PREFIX ||
                *cursor.ptr == AML_MULTI_NAME_PREFIX || *cursor.ptr == 0x00) {
                return skipNameString(cursor);
            }
            ++cursor.ptr;
            return true;
    }
}

bool Interpreter::skipTerm(Cursor& cursor) {
    return skipTermArg(cursor);
}

Object* Interpreter::allocateObjects(size_t count) {
    if (count == 0) {
        return nullptr;
    }
    Object* objects = reinterpret_cast<Object*>(kmalloc(sizeof(Object) * count));
    if (!objects) {
        return nullptr;
    }
    memset(objects, 0, sizeof(Object) * count);
    return objects;
}

bool Interpreter::makeInteger(uint64_t value, Object* result) {
    if (!result) {
        return false;
    }
    memset(result, 0, sizeof(Object));
    result->type = ObjectType::Integer;
    result->integer = value;
    return true;
}

bool Interpreter::copyObject(const Object& source, Object* dest) {
    if (!dest) {
        return false;
    }
    *dest = source;
    return true;
}

bool Interpreter::objectToInteger(const Object& object, uint64_t* value) {
    if (!value) {
        return false;
    }

    if (object.type == ObjectType::Integer) {
        *value = object.integer;
        return true;
    }
    if (object.type == ObjectType::Buffer) {
        uint64_t result = 0;
        const size_t bytes = object.length < 8 ? object.length : 8;
        for (size_t i = 0; i < bytes; ++i) {
            result |= static_cast<uint64_t>(object.buffer[i]) << (i * 8);
        }
        *value = result;
        return true;
    }
    if (object.type == ObjectType::String && object.string) {
        uint64_t result = 0;
        size_t i = 0;
        while (i < object.length && object.string[i] >= '0' && object.string[i] <= '9') {
            result = result * 10 + static_cast<uint64_t>(object.string[i] - '0');
            ++i;
        }
        *value = result;
        return i != 0;
    }
    if (object.type == ObjectType::Reference && object.node) {
        Object resolved;
        ExecutionContext context;
        context.interpreter = this;
        context.scope = object.node->parent ? object.node->parent : root;
        if (evaluateNamedObject(context, object.node, &resolved)) {
            return objectToInteger(resolved, value);
        }
    }

    return false;
}

bool Interpreter::evaluate(const char* path, Object* result) {
    if (!initialized || !result) {
        return false;
    }

    NamespaceNode* node = resolveAsciiPath(path, root, false);
    if (!node) {
        return false;
    }

    ExecutionContext context;
    context.interpreter = this;
    context.scope = node->parent ? node->parent : root;
    return evaluateNamedObject(context, node, result);
}

bool Interpreter::evaluateInteger(const char* path, uint64_t* result) {
    Object object;
    if (!evaluate(path, &object)) {
        return false;
    }
    return objectToInteger(object, result);
}

bool Interpreter::getS5SleepTypes(uint16_t* slpTypA, uint16_t* slpTypB) {
    if (!slpTypA || !slpTypB) {
        return false;
    }

    Object object;
    if (!evaluate("\\_S5", &object) && !evaluate("\\_S5_", &object) && !evaluate("_S5", &object)) {
        return false;
    }
    if (object.type != ObjectType::Package || object.elementCount < 2 || !object.elements) {
        return false;
    }

    uint64_t first = 0;
    uint64_t second = 0;
    if (!objectToInteger(object.elements[0], &first)) {
        return false;
    }
    if (!objectToInteger(object.elements[1], &second)) {
        second = first;
    }

    *slpTypA = static_cast<uint16_t>(first & 0x7);
    *slpTypB = static_cast<uint16_t>(second & 0x7);
    return true;
}

bool Interpreter::evaluateNamedObject(ExecutionContext& context, NamespaceNode* node, Object* result) {
    if (!node) {
        return false;
    }
    if (node->object.type == ObjectType::Alias && node->object.node) {
        return evaluateNamedObject(context, node->object.node, result);
    }
    if (node->object.type == ObjectType::Method) {
        return executeMethod(node, nullptr, 0, result, context.depth + 1);
    }
    return copyObject(node->object, result);
}

bool Interpreter::evaluateObject(ExecutionContext& context, Cursor& cursor, Object* result) {
    if (!result || cursor.ptr >= cursor.end) {
        return false;
    }

    const uint8_t* before = cursor.ptr;
    uint8_t op = *cursor.ptr++;
    switch (op) {
        case AML_ZERO_OP:
            return makeInteger(0, result);
        case AML_ONE_OP:
            return makeInteger(1, result);
        case AML_ONES_OP:
            return makeInteger(~0ULL, result);
        case AML_BYTE_PREFIX:
            if (cursor.ptr + 1 > cursor.end) return false;
            return makeInteger(*cursor.ptr++, result);
        case AML_WORD_PREFIX:
            if (cursor.ptr + 2 > cursor.end) return false;
            makeInteger(readLe16(cursor.ptr), result);
            cursor.ptr += 2;
            return true;
        case AML_DWORD_PREFIX:
            if (cursor.ptr + 4 > cursor.end) return false;
            makeInteger(readLe32(cursor.ptr), result);
            cursor.ptr += 4;
            return true;
        case AML_QWORD_PREFIX:
            if (cursor.ptr + 8 > cursor.end) return false;
            makeInteger(readLe64(cursor.ptr), result);
            cursor.ptr += 8;
            return true;
        case AML_STRING_PREFIX: {
            const char* string = reinterpret_cast<const char*>(cursor.ptr);
            size_t length = 0;
            while (cursor.ptr < cursor.end && *cursor.ptr != 0) {
                ++cursor.ptr;
                ++length;
            }
            if (cursor.ptr < cursor.end) {
                ++cursor.ptr;
            }
            memset(result, 0, sizeof(Object));
            result->type = ObjectType::String;
            result->string = string;
            result->length = length;
            return true;
        }
        case AML_BUFFER_OP: {
            const uint8_t* packageStart = nullptr;
            uint32_t packageLength = 0;
            if (!readPkgLength(cursor, &packageLength, &packageStart)) {
                return false;
            }
            const uint8_t* packageEnd = packageStart + packageLength;
            if (packageEnd > cursor.end || packageEnd < cursor.ptr) {
                return false;
            }
            Object sizeObject;
            uint64_t requestedSize = 0;
            if (!evaluateObject(context, cursor, &sizeObject) || !objectToInteger(sizeObject, &requestedSize)) {
                cursor.ptr = packageEnd;
                return false;
            }
            const size_t available = static_cast<size_t>(packageEnd - cursor.ptr);
            size_t bufferSize = static_cast<size_t>(requestedSize);
            if (bufferSize > available) {
                bufferSize = available;
            }
            memset(result, 0, sizeof(Object));
            result->type = ObjectType::Buffer;
            result->buffer = cursor.ptr;
            result->length = bufferSize;
            cursor.ptr = packageEnd;
            return true;
        }
        case AML_PACKAGE_OP:
        case AML_VAR_PACKAGE_OP: {
            const uint8_t* packageStart = nullptr;
            uint32_t packageLength = 0;
            if (!readPkgLength(cursor, &packageLength, &packageStart)) {
                return false;
            }
            const uint8_t* packageEnd = packageStart + packageLength;
            if (packageEnd > cursor.end || packageEnd < cursor.ptr) {
                return false;
            }

            uint64_t elementCount64 = 0;
            if (op == AML_PACKAGE_OP) {
                if (cursor.ptr >= packageEnd) {
                    return false;
                }
                elementCount64 = *cursor.ptr++;
            } else {
                Object countObject;
                if (!evaluateObject(context, cursor, &countObject) || !objectToInteger(countObject, &elementCount64)) {
                    cursor.ptr = packageEnd;
                    return false;
                }
            }

            size_t elementCount = static_cast<size_t>(elementCount64);
            Object* elements = allocateObjects(elementCount);
            if (elementCount != 0 && !elements) {
                cursor.ptr = packageEnd;
                return false;
            }

            size_t parsed = 0;
            while (parsed < elementCount && cursor.ptr < packageEnd) {
                if (!evaluateObject(context, cursor, &elements[parsed])) {
                    Cursor skipCursor = cursor;
                    if (!skipTermArg(skipCursor) || skipCursor.ptr <= cursor.ptr || skipCursor.ptr > packageEnd) {
                        break;
                    }
                    cursor = skipCursor;
                }
                ++parsed;
            }

            memset(result, 0, sizeof(Object));
            result->type = ObjectType::Package;
            result->elements = elements;
            result->elementCount = elementCount;
            cursor.ptr = packageEnd;
            return true;
        }
        case AML_STORE_OP: {
            Object value;
            if (!evaluateObject(context, cursor, &value)) {
                return false;
            }
            Object* targetObject = nullptr;
            NamespaceNode* targetNode = nullptr;
            if (!evaluateTarget(context, cursor, &targetObject, &targetNode)) {
                return false;
            }
            storeToTarget(value, targetObject, targetNode);
            return copyObject(value, result);
        }
        case AML_REF_OF_OP: {
            Object* targetObject = nullptr;
            NamespaceNode* targetNode = nullptr;
            if (!evaluateTarget(context, cursor, &targetObject, &targetNode)) {
                return false;
            }
            memset(result, 0, sizeof(Object));
            result->type = ObjectType::Reference;
            result->node = targetNode;
            if (targetObject && !targetNode) {
                result->elements = targetObject;
            }
            return true;
        }
        case AML_DEREF_OF_OP: {
            Object reference;
            if (!evaluateObject(context, cursor, &reference)) {
                return false;
            }
            if (reference.type == ObjectType::Reference && reference.node) {
                return evaluateNamedObject(context, reference.node, result);
            }
            if (reference.type == ObjectType::Reference && reference.elements) {
                return copyObject(*reference.elements, result);
            }
            return copyObject(reference, result);
        }
        case AML_ADD_OP:
        case AML_SUBTRACT_OP:
        case AML_MULTIPLY_OP:
        case AML_SHIFT_LEFT_OP:
        case AML_SHIFT_RIGHT_OP:
        case AML_AND_OP:
        case AML_NAND_OP:
        case AML_OR_OP:
        case AML_NOR_OP:
        case AML_XOR_OP:
        case AML_MOD_OP: {
            Object leftObject;
            Object rightObject;
            uint64_t left = 0;
            uint64_t right = 0;
            if (!evaluateObject(context, cursor, &leftObject) || !objectToInteger(leftObject, &left) ||
                !evaluateObject(context, cursor, &rightObject) || !objectToInteger(rightObject, &right)) {
                return false;
            }
            uint64_t value = 0;
            if (op == AML_ADD_OP) value = left + right;
            else if (op == AML_SUBTRACT_OP) value = left - right;
            else if (op == AML_MULTIPLY_OP) value = left * right;
            else if (op == AML_SHIFT_LEFT_OP) value = left << (right & 63);
            else if (op == AML_SHIFT_RIGHT_OP) value = left >> (right & 63);
            else if (op == AML_AND_OP) value = left & right;
            else if (op == AML_NAND_OP) value = ~(left & right);
            else if (op == AML_OR_OP) value = left | right;
            else if (op == AML_NOR_OP) value = ~(left | right);
            else if (op == AML_XOR_OP) value = left ^ right;
            else if (op == AML_MOD_OP) value = right == 0 ? 0 : (left % right);
            makeInteger(value, result);

            Object* targetObject = nullptr;
            NamespaceNode* targetNode = nullptr;
            if (evaluateTarget(context, cursor, &targetObject, &targetNode)) {
                storeToTarget(*result, targetObject, targetNode);
            }
            return true;
        }
        case AML_DIVIDE_OP: {
            Object dividendObject;
            Object divisorObject;
            uint64_t dividend = 0;
            uint64_t divisor = 0;
            if (!evaluateObject(context, cursor, &dividendObject) || !objectToInteger(dividendObject, &dividend) ||
                !evaluateObject(context, cursor, &divisorObject) || !objectToInteger(divisorObject, &divisor)) {
                return false;
            }
            Object remainder;
            Object quotient;
            makeInteger(divisor == 0 ? 0 : dividend % divisor, &remainder);
            makeInteger(divisor == 0 ? 0 : dividend / divisor, &quotient);
            Object* targetObject = nullptr;
            NamespaceNode* targetNode = nullptr;
            if (evaluateTarget(context, cursor, &targetObject, &targetNode)) {
                storeToTarget(remainder, targetObject, targetNode);
            }
            targetObject = nullptr;
            targetNode = nullptr;
            if (evaluateTarget(context, cursor, &targetObject, &targetNode)) {
                storeToTarget(quotient, targetObject, targetNode);
            }
            return copyObject(quotient, result);
        }
        case AML_INCREMENT_OP:
        case AML_DECREMENT_OP: {
            Object* targetObject = nullptr;
            NamespaceNode* targetNode = nullptr;
            if (!evaluateTarget(context, cursor, &targetObject, &targetNode)) {
                return false;
            }
            Object current = {};
            if (targetObject) {
                current = *targetObject;
            } else if (targetNode) {
                current = targetNode->object;
            }
            uint64_t value = 0;
            objectToInteger(current, &value);
            value = (op == AML_INCREMENT_OP) ? value + 1 : value - 1;
            makeInteger(value, result);
            storeToTarget(*result, targetObject, targetNode);
            return true;
        }
        case AML_NOT_OP:
        case AML_LNOT_OP:
        case AML_SIZE_OF_OP:
        case AML_TO_INTEGER_OP:
        case AML_OBJECT_TYPE_OP: {
            Object object;
            if (op == AML_OBJECT_TYPE_OP) {
                Object* targetObject = nullptr;
                NamespaceNode* targetNode = nullptr;
                if (!evaluateTarget(context, cursor, &targetObject, &targetNode)) {
                    return false;
                }
                ObjectType type = ObjectType::None;
                if (targetObject) {
                    type = targetObject->type;
                } else if (targetNode) {
                    type = targetNode->object.type;
                }
                return makeInteger(acpiObjectTypeValue(type), result);
            }
            if (!evaluateObject(context, cursor, &object)) {
                return false;
            }
            uint64_t value = 0;
            if (op == AML_SIZE_OF_OP) {
                if (object.type == ObjectType::String || object.type == ObjectType::Buffer) {
                    value = object.length;
                } else if (object.type == ObjectType::Package) {
                    value = object.elementCount;
                }
                return makeInteger(value, result);
            }
            if (!objectToInteger(object, &value)) {
                value = 0;
            }
            if (op == AML_NOT_OP) {
                return makeInteger(~value, result);
            }
            if (op == AML_LNOT_OP) {
                return makeInteger(value == 0 ? 1 : 0, result);
            }
            return makeInteger(value, result);
        }
        case AML_LAND_OP:
        case AML_LOR_OP:
        case AML_LEQUAL_OP:
        case AML_LGREATER_OP:
        case AML_LLESS_OP: {
            Object leftObject;
            Object rightObject;
            uint64_t left = 0;
            uint64_t right = 0;
            if (!evaluateObject(context, cursor, &leftObject) || !objectToInteger(leftObject, &left) ||
                !evaluateObject(context, cursor, &rightObject) || !objectToInteger(rightObject, &right)) {
                return false;
            }
            uint64_t value = 0;
            if (op == AML_LAND_OP) value = (left != 0 && right != 0) ? 1 : 0;
            else if (op == AML_LOR_OP) value = (left != 0 || right != 0) ? 1 : 0;
            else if (op == AML_LEQUAL_OP) value = (left == right) ? 1 : 0;
            else if (op == AML_LGREATER_OP) value = (left > right) ? 1 : 0;
            else if (op == AML_LLESS_OP) value = (left < right) ? 1 : 0;
            return makeInteger(value, result);
        }
        case AML_INDEX_OP: {
            Object source;
            Object indexObject;
            uint64_t index = 0;
            if (!evaluateObject(context, cursor, &source) ||
                !evaluateObject(context, cursor, &indexObject) ||
                !objectToInteger(indexObject, &index)) {
                return false;
            }
            Object reference = {};
            reference.type = ObjectType::Reference;
            if (source.type == ObjectType::Package && source.elements && index < source.elementCount) {
                reference.elements = &source.elements[index];
            }
            Object* targetObject = nullptr;
            NamespaceNode* targetNode = nullptr;
            if (evaluateTarget(context, cursor, &targetObject, &targetNode)) {
                storeToTarget(reference, targetObject, targetNode);
            }
            return copyObject(reference, result);
        }
        case AML_EXT_OP:
            if (cursor.ptr >= cursor.end) {
                return false;
            }
            switch (*cursor.ptr++) {
                case AML_EXT_REVISION_OP:
                    return makeInteger(2, result);
                case AML_EXT_TIMER_OP:
                    return makeInteger(0, result);
                case AML_EXT_COND_REF_OF_OP: {
                    Object* ignoredObject = nullptr;
                    NamespaceNode* targetNode = nullptr;
                    bool exists = evaluateTarget(context, cursor, &ignoredObject, &targetNode);
                    Object reference = {};
                    reference.type = ObjectType::Reference;
                    reference.node = targetNode;
                    Object* storeObject = nullptr;
                    NamespaceNode* storeNode = nullptr;
                    if (evaluateTarget(context, cursor, &storeObject, &storeNode)) {
                        storeToTarget(reference, storeObject, storeNode);
                    }
                    return makeInteger((exists && targetNode) ? 1 : 0, result);
                }
                case AML_EXT_STALL_OP:
                case AML_EXT_SLEEP_OP: {
                    Object ignored;
                    evaluateObject(context, cursor, &ignored);
                    return makeInteger(0, result);
                }
                case AML_EXT_ACQUIRE_OP:
                    skipSuperName(cursor);
                    skipTermArg(cursor);
                    return makeInteger(0, result);
                case AML_EXT_RELEASE_OP:
                    skipSuperName(cursor);
                    return makeInteger(0, result);
                case AML_EXT_DEBUG_OP:
                    memset(result, 0, sizeof(Object));
                    result->type = ObjectType::Reference;
                    return true;
                default:
                    return false;
            }
        default:
            cursor.ptr = before;
            if ((op >= AML_LOCAL0 && op <= AML_LOCAL0 + 7) || (op >= AML_ARG0 && op <= AML_ARG0 + 6)) {
                ++cursor.ptr;
                if (op >= AML_LOCAL0 && op <= AML_LOCAL0 + 7) {
                    return copyObject(context.locals[op - AML_LOCAL0], result);
                }
                return copyObject(context.args[op - AML_ARG0], result);
            }
            if (isNameLead(op) || op == AML_ROOT_CHAR || op == AML_PARENT_PREFIX_CHAR ||
                op == AML_DUAL_NAME_PREFIX || op == AML_MULTI_NAME_PREFIX || op == 0x00) {
                NamespaceNode* node = resolveNameString(cursor, context.scope, false);
                if (!node) {
                    return false;
                }
                if (node->object.type == ObjectType::Method) {
                    Object args[7];
                    memset(args, 0, sizeof(args));
                    const size_t argCount = node->object.methodArgCount;
                    for (size_t i = 0; i < argCount && i < 7; ++i) {
                        if (!evaluateObject(context, cursor, &args[i])) {
                            return false;
                        }
                    }
                    return executeMethod(node, args, argCount, result, context.depth + 1);
                }
                return evaluateNamedObject(context, node, result);
            }
            return false;
    }
}

bool Interpreter::evaluateTarget(ExecutionContext& context, Cursor& cursor, Object** targetObject,
                                 NamespaceNode** targetNode) {
    if (!targetObject || !targetNode || cursor.ptr >= cursor.end) {
        return false;
    }

    *targetObject = nullptr;
    *targetNode = nullptr;

    const uint8_t op = *cursor.ptr++;
    if (op == AML_ZERO_OP || op == AML_EXT_DEBUG_OP) {
        return true;
    }
    if (op >= AML_LOCAL0 && op <= AML_LOCAL0 + 7) {
        *targetObject = &context.locals[op - AML_LOCAL0];
        return true;
    }
    if (op >= AML_ARG0 && op <= AML_ARG0 + 6) {
        *targetObject = &context.args[op - AML_ARG0];
        return true;
    }
    if (op == AML_INDEX_OP) {
        Object source;
        Object indexObject;
        uint64_t index = 0;
        if (!evaluateObject(context, cursor, &source) ||
            !evaluateObject(context, cursor, &indexObject) ||
            !objectToInteger(indexObject, &index)) {
            return false;
        }
        skipSuperName(cursor);
        if (source.type == ObjectType::Package && source.elements && index < source.elementCount) {
            *targetObject = &source.elements[index];
        }
        return true;
    }

    --cursor.ptr;
    NamespaceNode* node = resolveNameString(cursor, context.scope, true);
    if (!node) {
        return false;
    }
    *targetNode = node;
    return true;
}

bool Interpreter::storeToTarget(const Object& value, Object* targetObject, NamespaceNode* targetNode) {
    if (targetObject) {
        *targetObject = value;
        return true;
    }
    if (targetNode) {
        targetNode->object = value;
        targetNode->defined = true;
        return true;
    }
    return true;
}

bool Interpreter::executeMethod(NamespaceNode* methodNode, const Object* args, size_t argCount, Object* result,
                                uint8_t depth) {
    if (!methodNode || methodNode->object.type != ObjectType::Method || !result || depth > kMaxMethodDepth) {
        return false;
    }

    if (!methodNode->object.methodBody && sameSegment(methodNode->name, "_OSI")) {
        return makeInteger(0, result);
    }

    ExecutionContext context;
    context.interpreter = this;
    context.scope = methodNode->parent ? methodNode->parent : root;
    context.depth = depth;
    for (size_t i = 0; i < argCount && i < 7; ++i) {
        context.args[i] = args[i];
    }

    Cursor cursor;
    cursor.ptr = methodNode->object.methodBody;
    cursor.end = methodNode->object.methodBody + methodNode->object.methodLength;
    if (!executeTermList(context, cursor, cursor.end)) {
        return false;
    }

    if (context.returned) {
        return copyObject(context.returnValue, result);
    }
    memset(result, 0, sizeof(Object));
    result->type = ObjectType::None;
    return true;
}

bool Interpreter::executeTermList(ExecutionContext& context, Cursor& cursor, const uint8_t* listEnd) {
    while (cursor.ptr < listEnd && cursor.ptr < cursor.end && !context.returned && !context.broke) {
        const uint8_t* before = cursor.ptr;
        const uint8_t op = *cursor.ptr++;

        if (op == AML_RETURN_OP) {
            if (!evaluateObject(context, cursor, &context.returnValue)) {
                memset(&context.returnValue, 0, sizeof(Object));
            }
            context.returned = true;
            return true;
        }
        if (op == AML_BREAK_OP) {
            context.broke = true;
            return true;
        }
        if (op == AML_CONTINUE_OP) {
            return true;
        }
        if (op == AML_NOOP_OP) {
            continue;
        }
        if (op == AML_IF_OP) {
            const uint8_t* packageStart = nullptr;
            uint32_t packageLength = 0;
            if (!readPkgLength(cursor, &packageLength, &packageStart)) {
                return false;
            }
            const uint8_t* packageEnd = packageStart + packageLength;
            if (packageEnd > cursor.end || packageEnd > listEnd) {
                return false;
            }

            Object predicateObject;
            uint64_t predicate = 0;
            if (!evaluateObject(context, cursor, &predicateObject) ||
                !objectToInteger(predicateObject, &predicate)) {
                predicate = 0;
            }

            if (predicate != 0) {
                Cursor body;
                body.ptr = cursor.ptr;
                body.end = packageEnd;
                executeTermList(context, body, packageEnd);
            }
            cursor.ptr = packageEnd;

            if (cursor.ptr < listEnd && cursor.ptr < cursor.end && *cursor.ptr == AML_ELSE_OP) {
                ++cursor.ptr;
                const uint8_t* elsePackageStart = nullptr;
                uint32_t elseLength = 0;
                if (!readPkgLength(cursor, &elseLength, &elsePackageStart)) {
                    return false;
                }
                const uint8_t* elseEnd = elsePackageStart + elseLength;
                if (elseEnd > cursor.end || elseEnd > listEnd) {
                    return false;
                }
                if (predicate == 0 && !context.returned && !context.broke) {
                    Cursor elseBody;
                    elseBody.ptr = cursor.ptr;
                    elseBody.end = elseEnd;
                    executeTermList(context, elseBody, elseEnd);
                }
                cursor.ptr = elseEnd;
            }
            continue;
        }
        if (op == AML_ELSE_OP) {
            const uint8_t* packageStart = nullptr;
            uint32_t packageLength = 0;
            if (!readPkgLength(cursor, &packageLength, &packageStart)) {
                return false;
            }
            cursor.ptr = packageStart + packageLength;
            if (cursor.ptr > cursor.end) {
                return false;
            }
            continue;
        }
        if (op == AML_WHILE_OP) {
            const uint8_t* packageStart = nullptr;
            uint32_t packageLength = 0;
            if (!readPkgLength(cursor, &packageLength, &packageStart)) {
                return false;
            }
            const uint8_t* packageEnd = packageStart + packageLength;
            if (packageEnd > cursor.end || packageEnd > listEnd) {
                return false;
            }
            const uint8_t* predicateStart = cursor.ptr;
            for (uint32_t iteration = 0; iteration < kMaxWhileIterations; ++iteration) {
                Cursor predicateCursor;
                predicateCursor.ptr = predicateStart;
                predicateCursor.end = packageEnd;
                Object predicateObject;
                uint64_t predicate = 0;
                if (!evaluateObject(context, predicateCursor, &predicateObject) ||
                    !objectToInteger(predicateObject, &predicate) || predicate == 0) {
                    break;
                }
                Cursor body;
                body.ptr = predicateCursor.ptr;
                body.end = packageEnd;
                executeTermList(context, body, packageEnd);
                if (context.returned) {
                    break;
                }
                if (context.broke) {
                    context.broke = false;
                    break;
                }
            }
            cursor.ptr = packageEnd;
            continue;
        }

        cursor.ptr = before;
        Object ignored;
        if (!evaluateObject(context, cursor, &ignored)) {
            cursor.ptr = before;
            if (!skipTerm(cursor) || cursor.ptr <= before) {
                cursor.ptr = before + 1;
            }
        }
    }
    return true;
}

}
