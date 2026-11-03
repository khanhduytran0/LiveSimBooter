#pragma once

// The layouts are from make_subshell_command (+0x11cf4), command_connect (+0x10e74) and execute_in_subshell (+0x18a10)
// in the pinned Bash. The latter unwraps COMMAND.value at +0x18c38 and SUBSHELL_COM.command at +0x18c3c.
enum { BashSimpleType = 4, BashConnectionType = 6, BashSubshellType = 13, BashAndAnd = 0x11d, BashOrOr = 0x11e, BashMaxGroupedCommands = 64 };
typedef struct { int32_t type, flags, line, padding; uintptr_t redirects, value; } BashCommandNode;
typedef struct { int32_t flags, line; uintptr_t command; } BashSubshellNode;
typedef struct { int32_t flags, padding; uintptr_t first, second; int32_t connector, spare; } BashConnectionNode;
_Static_assert(sizeof(BashCommandNode) == 32 && offsetof(BashCommandNode, value) == 0x18, "Bash COMMAND");
_Static_assert(sizeof(BashSubshellNode) == 16 && offsetof(BashSubshellNode, command) == 8, "Bash SUBSHELL_COM");
_Static_assert(sizeof(BashConnectionNode) == 32 && offsetof(BashConnectionNode, connector) == 0x18, "Bash CONNECTION");

// A grouped command gets its own Bash top-level exit landing. Bound the syntax tree, including malformed cycles,
// before spawning. Simple commands use the same word and redirect inventory as the pipe/async branch.
static int addBashGroupedBody(TrialPlan *plan, uintptr_t image, uintptr_t address, size_t page, unsigned *remaining) {
    if (!*remaining) return E2BIG;
    --*remaining;
    BashCommandNode node;
    int error = addBashObject(plan, address, &node, sizeof(node), page);
    if (!error) error = addBashRedirectPages(plan, image, node.redirects, page);
    if (error) return error;
    if (node.type == BashSimpleType) return addBashSimpleCommand(plan, image, node.value, page);
    if (node.type == BashSubshellType) {
        // execute_in_subshell recognizes subshell, invert, ignore-return, no-fork and asynchronous-stdin flags.
        // Other flags (e.g. timing wrappers) select additional interpreter paths outside this inventory.
        enum { GroupFlags = 0x1 | 0x2 | 0x4 | 0x8 | 0x40 | 0x400 };
        if (node.flags & ~GroupFlags) return ENOTSUP;
        BashSubshellNode group;
        if ((error = addBashObject(plan, node.value, &group, sizeof(group), page))) return error;
        return addBashGroupedBody(plan, image, group.command, page, remaining);
    }
    if (node.type != BashConnectionType) return ENOTSUP;
    BashConnectionNode connection;
    if ((error = addBashObject(plan, node.value, &connection, sizeof(connection), page))) return error;
    if (connection.connector != ';' && connection.connector != BashAndAnd && connection.connector != BashOrOr) return ENOTSUP;
    if ((error = addBashGroupedBody(plan, image, connection.first, page, remaining))) return error;
    return addBashGroupedBody(plan, image, connection.second, page, remaining);
}

static int addBashSubshellCommand(TrialPlan *plan, TrialOffer *offer, const BashMakeChildFrame *frame, size_t page) {
    const uintptr_t image = offer->mainHeader;
    // execute_command_internal saves the COMMAND in x19 and passes it as the first argument to execute_in_subshell at +0x188cc.
    BashCommandNode outer;
    int error = addBashObject(plan, frame->x19, &outer, sizeof(outer), page);
    if (error) return error;
    if (outer.type != BashSubshellType) return ENOTSUP; // Forced forks for other compound command types use the same call site.
    int32_t trapMode, returnCatch;
    if (!forkTrialRead(image + BashSignalModes, &trapMode, sizeof(trapMode)) || !forkTrialRead(image + 0xa6e44, &returnCatch, sizeof(returnCatch))) return EFAULT;
    // An enclosing function's return_catch may contain a parent-owned jump buffer. Also exclude inherited EXIT traps.
    if (returnCatch || (trapMode & 1)) return ENOTSUP;
    int pipeIn = (int32_t)offer->caller.x19ToX28[6], pipeOut = (int32_t)frame->x23, async = (int32_t)frame->x22;
    if (pipeIn < -1 || pipeOut < -1 || (async != 0 && async != 1)) return ENOTSUP;
    if (pipeIn >= 0 && (error = preserveBashPipeFD(offer, pipeIn))) return error;
    if (pipeOut >= 0 && (error = preserveBashPipeFD(offer, pipeOut))) return error;
    if ((error = addBashCloseBitmap(plan, offer, frame->x24, page))) return error;
    unsigned remaining = BashMaxGroupedCommands;
    return addBashGroupedBody(plan, image, frame->x19, page, &remaining);
}
