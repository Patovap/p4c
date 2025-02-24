/*
Copyright 2016 VMware, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "simplifyDefUse.h"
#include "frontends/p4/def_use.h"
#include "frontends/p4/methodInstance.h"
#include "frontends/p4/tableApply.h"
#include "frontends/p4/sideEffects.h"

namespace P4 {

namespace {

class HasUses {
    // Set of program points whose left-hand sides are used elsewhere
    // in the program together with their use count
    std::set<const IR::Node*> used;

    class SliceTracker {
        const IR::Slice* trackedSlice = nullptr;
        bool active = false;
        bool overwritesPrevious(const IR::Slice *previous) {
            if (trackedSlice->getH() >= previous->getH() &&
                trackedSlice->getL() <= previous->getL())
                // current overwrites the previous
                return true;

            return false;
        }

     public:
        SliceTracker() = default;
        explicit SliceTracker(const IR::Slice* slice) :
            trackedSlice(slice), active(true) { }
        bool isActive() const { return active; }

        // main logic of this class
        bool overwrites(const ProgramPoint previous) {
            if (!isActive()) return false;
            if (previous.isBeforeStart()) return false;
            auto last = previous.last();
            if (auto *assign_stmt = last->to<IR::AssignmentStatement>()) {
                if (auto *slice_stmt = assign_stmt->left->to<IR::Slice>()) {
                    // two slice stmts writing to same location
                    // skip use of previous if it gets overwritten
                    if (overwritesPrevious(slice_stmt)) {
                        LOG4("Skipping " << dbp(last) << " " << last);
                        return true;
                    }
                }
            }

            return false;
        }
    };

    SliceTracker tracker;

 public:
    HasUses() = default;
    void add(const ProgramPoints* points) {
        for (auto e : *points) {
            // skips overwritten slice statements
            if (tracker.overwrites(e)) continue;

            auto last = e.last();
            if (last != nullptr) {
                LOG3("Found use for " << dbp(last) << " " <<
                     (last->is<IR::Statement>() ? last : nullptr));
                used.emplace(last);
            }
        }
    }
    bool hasUses(const IR::Node* node) const
    { return used.find(node) != used.end(); }

    void watchForOverwrites(const IR::Slice* slice) {
        BUG_CHECK(!tracker.isActive(), "Call to SliceTracker, but it's already active");
        tracker = SliceTracker(slice);
    }

    void doneWatching() {
        tracker = SliceTracker();
    }
};

// Run for each parser and control separately
// Somewhat of a misnamed pass -- the main point of this pass is to find all the uses
// of each definition, and fill in the `hasUses` output with all the definitions that have
// uses so RemoveUnused can remove unused things.  It incidentally finds uses that have
// no definitions and issues uninitialized warnings about them.
class FindUninitialized : public Inspector {
    ProgramPoint    context;    // context as of the last call or state transition
    ReferenceMap*   refMap;
    TypeMap*        typeMap;
    AllDefinitions* definitions;
    bool            lhs;  // checking the lhs of an assignment
    ProgramPoint    currentPoint;  // context of the current expression/statement
    /// For some simple expresssions keep here the read location sets.
    /// This does not include location sets read by subexpressions.
    std::map<const IR::Expression*, const LocationSet*> readLocations;
    HasUses*        hasUses;  // output
    /// If true the current statement is unreachable
    bool            unreachable;
    bool            virtualMethod;

    const LocationSet* getReads(const IR::Expression* expression, bool nonNull = false) const {
        auto result = ::get(readLocations, expression);
        if (nonNull)
            BUG_CHECK(result != nullptr, "no locations known for %1%", dbp(expression));
        return result;
    }
    /// 'expression' is reading the 'loc' location set
    void reads(const IR::Expression* expression, const LocationSet* loc) {
        BUG_CHECK(!unreachable, "reached an unreachable expression in FindUninitialized");
        LOG3(expression << " reads " << loc);
        CHECK_NULL(expression);
        CHECK_NULL(loc);
        readLocations.erase(expression);
        readLocations.emplace(expression, loc);
    }
    bool setCurrent(const IR::Statement* statement) {
        currentPoint = ProgramPoint(context, statement);
        LOG3(IndentCtl::unindent);
        return false;
    }
    profile_t init_apply(const IR::Node *root) override {
        unreachable = false;  // assume not unreachable at the start of any apply
        return Inspector::init_apply(root);
    }

    FindUninitialized(FindUninitialized* parent, ProgramPoint context) :
            context(context), refMap(parent->definitions->storageMap->refMap),
            typeMap(parent->definitions->storageMap->typeMap),
            definitions(parent->definitions), lhs(false), currentPoint(context),
            hasUses(parent->hasUses), virtualMethod(false) { visitDagOnce = false; }

 public:
    FindUninitialized(AllDefinitions* definitions, HasUses* hasUses) :
            refMap(definitions->storageMap->refMap),
            typeMap(definitions->storageMap->typeMap),
            definitions(definitions), lhs(false), currentPoint(),
            hasUses(hasUses), virtualMethod(false) {
        CHECK_NULL(refMap); CHECK_NULL(typeMap); CHECK_NULL(definitions);
        CHECK_NULL(hasUses);
        visitDagOnce = false; }

    // we control the traversal order manually, so we always 'prune()'
    // (return false from preorder)

    bool preorder(const IR::ParserState* state) override {
        LOG3("FU Visiting state " << state->name);
        context = ProgramPoint(state);
        currentPoint = ProgramPoint(state);  // point before the first statement
        visit(state->components, "components");
        if (state->selectExpression != nullptr)
            visit(state->selectExpression);
        context = ProgramPoint();
        return false;
    }

    Definitions* getCurrentDefinitions() const {
        auto defs = definitions->getDefinitions(currentPoint, true);
        LOG3("FU Current point is (after) " << currentPoint <<
                " definitions are " << IndentCtl::endl << defs);
        return defs;
    }

    void checkOutParameters(const IR::IDeclaration* block,
                            const IR::ParameterList* parameters,
                            Definitions* defs) {
        LOG2("Checking output parameters; definitions are " << IndentCtl::endl << defs);
        for (auto p : parameters->parameters) {
            if (p->direction == IR::Direction::Out || p->direction == IR::Direction::InOut) {
                auto storage = definitions->storageMap->getStorage(p);
                LOG3("Checking parameter: " << p);
                if (storage == nullptr)
                    continue;

                const LocationSet* loc = new LocationSet(storage);
                auto points = defs->getPoints(loc);
                hasUses->add(points);
                if (typeMap->typeIsEmpty(storage->type))
                    continue;
                // Check uninitialized non-headers (headers can be invalid).
                // inout parameters can never match here, so we could skip them.
                loc = storage->removeHeaders();
                points = defs->getPoints(loc);
                if (points->containsBeforeStart())
                    ::warning(ErrorType::WARN_UNINITIALIZED_OUT_PARAM,
                              "out parameter '%1%' may be uninitialized when "
                              "'%2%' terminates", p, block->getName());
            }
        }
    }

    bool preorder(const IR::P4Control* control) override {
        LOG3("FU Visiting control " << control->name << "[" << control->id << "]");
        BUG_CHECK(context.isBeforeStart(), "non-empty context in FindUnitialized::P4Control");
        currentPoint = ProgramPoint(control);
        visitVirtualMethods(control->controlLocals);
        unreachable = false;
        visit(control->body);
        checkOutParameters(
            control, control->getApplyMethodType()->parameters, getCurrentDefinitions());
        LOG3("FU Returning from " << control->name << "[" << control->id << "]");
        return false;
    }

    bool preorder(const IR::Function* func) override {
        if (virtualMethod) {
            LOG3("Virtual method");
            context = ProgramPoint::beforeStart;
            unreachable = false;
        }
        LOG3("FU Visiting function " << dbp(func) << " called by " << context);
        LOG5(func);
        auto point = ProgramPoint(context, func);
        currentPoint = point;
        visit(func->body);
        bool checkReturn = !func->type->returnType->is<IR::Type_Void>();
        if (checkReturn) {
            auto defs = getCurrentDefinitions();
            // The definitions after the body of the function should
            // contain "unreachable", otherwise it means that we have
            // not executed a 'return' on all possible paths.
            if (!defs->isUnreachable())
                ::error(ErrorType::ERR_INSUFFICIENT,
                        "Function '%1%' does not return a value on all paths", func);
        }

        currentPoint = point.after();
        // We now check the out parameters using the definitions
        // produced *after* the function has completed.
        LOG3("Context after function " << currentPoint);
        auto current = getCurrentDefinitions();
        checkOutParameters(func, func->type->parameters, current);
        return false;
    }

    void visitVirtualMethods(const IR::IndexedVector<IR::Declaration> &locals) {
        // We don't really know when virtual methods may be called, so
        // we visit them proactively once as if they are top-level functions.
        // During this visit the 'virtualMethod' flag is 'true'.
        // We may visit them also when they are invoked by a callee, but
        // at that time the 'virtualMethod' flag will be false.
        auto saveContext = context;
        for (auto l : locals) {
            if (auto li = l->to<IR::Declaration_Instance>()) {
                if (li->initializer) {
                    virtualMethod = true;
                    visit(li->initializer);
                    virtualMethod = false;
                }}}
        context = saveContext;
    }

    bool preorder(const IR::P4Parser* parser) override {
        LOG3("FU Visiting parser " << parser->name << "[" << parser->id << "]");
        currentPoint = ProgramPoint(parser);
        visitVirtualMethods(parser->parserLocals);
        visit(parser->states, "states");
        unreachable = false;
        auto accept = ProgramPoint(parser->getDeclByName(IR::ParserState::accept)->getNode());
        auto acceptdefs = definitions->getDefinitions(accept, true);
        auto reject = ProgramPoint(parser->getDeclByName(IR::ParserState::reject)->getNode());
        auto rejectdefs = definitions->getDefinitions(reject, true);

        auto outputDefs = acceptdefs->joinDefinitions(rejectdefs);
        checkOutParameters(parser, parser->getApplyMethodType()->parameters, outputDefs);
        LOG3("FU Returning from " << parser->name << "[" << parser->id << "]");
        return false;
    }

    // expr is an sub-expression that appears in the lhs of an assignment.
    // parent is one of it's parent expressions.
    //
    // When we assign to a header we are also implicitly reading the header's
    // valid flag.
    // Consider this example:
    // header H { ... };
    // H a;
    // a.x = 1;  <<< This has an effect only if a is valid.
    //               So this write actually reads the valid flag of a.
    // The function will recurse the structure of expr until it finds
    // a header and will mark the header valid bit as read.
    // It returns the LocationSet of parent.
    const LocationSet* checkHeaderFieldWrite(
        const IR::Expression* expr, const IR::Expression* parent) {
        const LocationSet* loc;
        if (auto mem = parent->to<IR::Member>()) {
            loc = checkHeaderFieldWrite(expr, mem->expr);
            loc = loc->getField(mem->member);
        } else if (auto ai = parent->to<IR::ArrayIndex>()) {
            loc = checkHeaderFieldWrite(expr, ai->left);
            if (auto cst = ai->right->to<IR::Constant>()) {
                auto index = cst->asInt();
                loc = loc->getIndex(index);
            }
            // else let loc be the whole array
        } else if (auto pe = parent->to<IR::PathExpression>()) {
            auto decl = refMap->getDeclaration(pe->path, true);
            auto storage = definitions->storageMap->getStorage(decl);
            if (storage != nullptr)
                loc = new LocationSet(storage);
            else
                loc = LocationSet::empty;
        } else if (auto slice = parent->to<IR::Slice>()) {
            loc = checkHeaderFieldWrite(expr, slice->e0);
        } else {
            BUG("%1%: unexpected expression on LHS", parent);
        }

        auto type = typeMap->getType(parent, true);
        if (type->is<IR::Type_Header>()) {
            if (expr != parent) {
                // If we are writing to an entire header (expr ==
                // parent) we are actually overwriting the valid bit
                // as well.  So we are not reading it.
                loc = loc->getValidField();
                LOG3("Expression " << expr << " reads valid bit " << loc);
                reads(expr, loc);
                registerUses(expr);
            }
        }
        return loc;
    }

    bool preorder(const IR::AssignmentStatement* statement) override {
        LOG3("FU Visiting " << dbp(statement) << " " << statement << IndentCtl::indent);
        if (!unreachable) {
            lhs = true;
            visit(statement->left);
            checkHeaderFieldWrite(statement->left, statement->left);
            LOG3("FU Returned from " << statement->left);
            lhs = false;
            visit(statement->right);
            LOG3("FU Returned from " << statement->right);
        } else {
            LOG3("Unreachable");
        }
        return setCurrent(statement);
    }

    bool preorder(const IR::ReturnStatement* statement) override {
        LOG3("FU Visiting " << statement);
        if (!unreachable && statement->expression != nullptr)
            visit(statement->expression);
        else
            LOG3("Unreachable");
        unreachable = true;
        return setCurrent(statement);
    }

    bool preorder(const IR::ExitStatement* statement) override {
        LOG3("FU Visiting " << statement);
        unreachable = true;
        LOG3("Unreachable");
        return setCurrent(statement);
    }

    bool preorder(const IR::MethodCallStatement* statement) override {
        LOG3("FU Visiting " << statement);
        if (!unreachable)
            visit(statement->methodCall);
        else
            LOG3("Unreachable");

        return setCurrent(statement);
    }

    bool preorder(const IR::BlockStatement* statement) override {
        LOG3("FU Visiting " << statement);
        if (!unreachable) {
            visit(statement->components, "components");
        } else {
            LOG3("Unreachable");
        }
        return setCurrent(statement);
    }

    bool preorder(const IR::IfStatement* statement) override {
        LOG3("FU Visiting " << statement);
        if (!unreachable) {
            visit(statement->condition);
            currentPoint = ProgramPoint(context, statement->condition);
            auto saveCurrent = currentPoint;
            auto saveUnreachable = unreachable;
            visit(statement->ifTrue);
            auto unreachableAfterThen = unreachable;
            unreachable = saveUnreachable;
            if (statement->ifFalse != nullptr) {
                currentPoint = saveCurrent;
                visit(statement->ifFalse);
            }
            unreachable = unreachableAfterThen && unreachable;
        } else {
            LOG3("Unreachable");
        }
        return setCurrent(statement);
    }

    bool preorder(const IR::SwitchStatement* statement) override {
        LOG3("FU Visiting " << statement);
        if (!unreachable) {
            bool finalUnreachable = true;
            visit(statement->expression);
            currentPoint = ProgramPoint(context, statement->expression);
            auto saveCurrent = currentPoint;
            auto saveUnreachable = unreachable;
            for (auto c : statement->cases) {
                if (c->statement != nullptr) {
                    LOG3("Visiting " << c);
                    currentPoint = saveCurrent;
                    unreachable = saveUnreachable;
                    visit(c);
                    finalUnreachable = finalUnreachable && unreachable;
                }
            }
            unreachable = finalUnreachable;
        } else {
            LOG3("Unreachable");
        }
        return setCurrent(statement);
    }

    ////////////////// Expressions

    bool preorder(const IR::Literal* expression) override {
        reads(expression, LocationSet::empty);
        return false;
    }

    bool preorder(const IR::TypeNameExpression* expression) override {
        reads(expression, LocationSet::empty);
        return false;
    }

    // Check whether the expression the child of a Member or
    // ArrayIndex.  I.e., for and expression such as a.x within a
    // larger expression a.x.b it returns "false".  This is because
    // the expression is not reading a.x, it is reading just a.x.b.
    // ctx must be the context of the current expression in the
    // visitor.
    bool isFinalRead(const Visitor::Context* ctx, const IR::Expression* expression) {
        if (ctx == nullptr)
            return true;

        // If this expression is a child of a Member of a left
        // child of an ArrayIndex then we don't report it here, only
        // in the parent.
        auto parentexp = ctx->node->to<IR::Expression>();
        if (parentexp != nullptr) {
            if (parentexp->is<IR::Member>())
                return false;
            if (parentexp->is<IR::ArrayIndex>()) {
                // Since we are doing the visit using a custom order,
                // ctx->child_index is not accurate, so we check
                // manually whether this is the left child.
                auto ai = parentexp->to<IR::ArrayIndex>();
                if (ai->left == expression)
                    return false;
            }
        }
        return true;
    }

    // Keeps track of which expression producers have uses in the given expression
    void registerUses(const IR::Expression* expression, bool reportUninitialized = true) {
        LOG3("FU Registering uses for '" << expression << "'");
        if (!isFinalRead(getContext(), expression)) {
            LOG3("Expression '" << expression << "' is not fully read. Returning...");
            return;
        }

        auto currentDefinitions = getCurrentDefinitions();
        if (currentDefinitions->isUnreachable()) {
            LOG3("are not reachable. Returning...");
            return;
        }

        const LocationSet* read = getReads(expression);
        if (read == nullptr || read->isEmpty()) {
            LOG3("No LocationSet for '" << expression << "'. Returning...");
            return;
        }
        LOG3("LocationSet for '" << expression << "' is <<" << read << ">>");

        auto points = currentDefinitions->getPoints(read);
        if (reportUninitialized && !lhs && points->containsBeforeStart()) {
            // Do not report uninitialized values on the LHS.
            // This could happen if we are writing to an array element
            // with an unknown index.
            auto type = typeMap->getType(expression, true);
            cstring message;
            if (type->is<IR::Type_Base>())
                message = "%1% may be uninitialized";
            else
                message = "%1% may not be completely initialized";
            ::warning(ErrorType::WARN_UNINITIALIZED_USE, message, expression);
        }

        hasUses->add(points);
    }

    // For the following we compute the read set and save it.
    // We check the read set later.
    bool preorder(const IR::PathExpression* expression) override {
        LOG3("FU Visiting [" << expression->id << "]: " << expression);
        if (lhs) {
            reads(expression, LocationSet::empty);
            return false;
        }
        auto decl = refMap->getDeclaration(expression->path, true);
        LOG4("Declaration for path '" << expression->path << "' is "
            << IndentCtl::indent << IndentCtl::endl << decl
            << IndentCtl::unindent);

        auto storage = definitions->storageMap->getStorage(decl);
        const LocationSet* result;
        if (storage != nullptr)
            result = new LocationSet(storage);
        else
            result = LocationSet::empty;

        LOG4("LocationSet for declaration " << IndentCtl::indent << IndentCtl::endl << decl
            << IndentCtl::unindent << IndentCtl::endl << "is <<" << result << ">>");
        reads(expression, result);
        registerUses(expression);
        return false;
    }

    bool preorder(const IR::P4Action* action) override {
        BUG_CHECK(findContext<IR::P4Program>() == nullptr, "Unexpected action");
        LOG3("FU Visiting action " << action);
        unreachable = false;
        currentPoint = ProgramPoint(context, action);
        visit(action->body);
        checkOutParameters(action, action->parameters, getCurrentDefinitions());
        LOG3("FU Returning from " << action);
        return false;
    }

    bool preorder(const IR::P4Table* table) override {
        LOG3("FU Visiting " << table->name);
        auto savePoint = ProgramPoint(context, table);
        currentPoint = savePoint;
        auto key = table->getKey();
        visit(key);
        auto actions = table->getActionList();
        for (auto ale : actions->actionList) {
            BUG_CHECK(ale->expression->is<IR::MethodCallExpression>(),
                      "%1%: unexpected entry in action list", ale);
            visit(ale->expression);
            currentPoint = savePoint;  // restore the current point
                                    // it is modified by the inter-procedural analysis
        }
        LOG3("FU Returning from " << table->name);
        return false;
    }

    bool preorder(const IR::MethodCallExpression* expression) override {
        LOG3("FU Visiting [" << expression->id << "]: " << expression);
        visit(expression->method);
        auto mi = MethodInstance::resolve(expression, refMap, typeMap);
        if (auto bim = mi->to<BuiltInMethod>()) {
            auto base = getReads(bim->appliedTo, true);
            cstring name = bim->name.name;
            if (name == IR::Type_Stack::push_front ||
                name == IR::Type_Stack::pop_front) {
                // Reads all array fields
                reads(expression, base);
                registerUses(expression, false);
                return false;
            } else if (name == IR::Type_Header::isValid) {
                auto storage = base->getField(StorageFactory::validFieldName);
                reads(expression, storage);
                registerUses(expression);
                return false;
            }
        }

        // The effect of copy-in: in arguments are read
        LOG3("Summarizing call effect on in arguments; definitions are " << IndentCtl::endl <<
             getCurrentDefinitions());
        for (auto p : *mi->substitution.getParametersInArgumentOrder()) {
            auto expr = mi->substitution.lookup(p);
            if (p->direction != IR::Direction::Out) {
                visit(expr);
            }
        }

        // Symbolically call some methods (actions and tables, extern methods)
        std::vector <const IR::IDeclaration *> callee;
        if (auto ac = mi->to<ActionCall>()) {
            callee.push_back(ac->action);
        } else if (mi->isApply()) {
            auto am = mi->to<ApplyMethod>();
            if (am->isTableApply()) {
                auto table = am->object->to<IR::P4Table>();
                callee.push_back(table);
            }
        } else if (auto em = mi->to<ExternMethod>()) {
            LOG4("##call to extern " << expression);
            callee = em->mayCall(); }

        // We skip control and function apply calls, since we can
        // summarize their effects by assuming they write all out
        // parameters and read all in parameters and have no other
        // side effects.

        if (!callee.empty()) {
            LOG3("Analyzing " << callee << IndentCtl::indent);
            ProgramPoint pt(context, expression);
            FindUninitialized fu(this, pt);
            for (auto c : callee)
                (void)c->getNode()->apply(fu);
        }
        for (auto p : *mi->substitution.getParametersInArgumentOrder()) {
            auto expr = mi->substitution.lookup(p);
            if (p->direction == IR::Direction::Out ||
                p->direction == IR::Direction::InOut) {
                bool save = lhs;
                lhs = true;
                visit(expr);
                lhs = save;
            }
        }
        reads(expression, LocationSet::empty);
        return false;
    }

    bool preorder(const IR::Member* expression) override {
        LOG3("FU Visiting [" << expression->id << "]: " << expression);
        visit(expression->expr);
        LOG3("FU Returned from " << expression->expr);
        if (expression->expr->is<IR::TypeNameExpression>()) {
            // this is a constant
            reads(expression, LocationSet::empty);
            return false;
        }
        if (TableApplySolver::isHit(expression, refMap, typeMap) ||
            TableApplySolver::isActionRun(expression, refMap, typeMap))
            return false;

        auto type = typeMap->getType(expression, true);
        if (type->is<IR::Type_Method>())
            // dealt within the parent
            return false;

        auto storage = getReads(expression->expr, true);

        auto basetype = typeMap->getType(expression->expr, true);
        if (basetype->is<IR::Type_Stack>()) {
            if (expression->member.name == IR::Type_Stack::next ||
                expression->member.name == IR::Type_Stack::last) {
                reads(expression, storage);
                registerUses(expression, false);
                if (!lhs && expression->member.name == IR::Type_Stack::next)
                    ::warning(ErrorType::WARN_UNINITIALIZED,
                              "%1%: reading uninitialized value", expression);
                return false;
            } else if (expression->member.name == IR::Type_Stack::lastIndex) {
                auto index = storage->getArrayLastIndex();
                reads(expression, index);
                registerUses(expression, false);
                return false;
            }
        }

        auto fields = storage->getField(expression->member);
        reads(expression, fields);
        registerUses(expression);
        return false;
    }

    bool preorder(const IR::Slice* expression) override {
        LOG3("FU Visiting [" << expression->id << "]: " << expression);

        auto* slice_stmt = findContext<IR::AssignmentStatement>();
        if (slice_stmt != nullptr && lhs) {
            // track this slice statement
            hasUses->watchForOverwrites(expression);
            LOG4("Tracking " << dbp(slice_stmt) << " " << slice_stmt <<
                    " for potential overwrites"); }

        bool save = lhs;
        lhs = false;  // slices on the LHS also read the data
        visit(expression->e0);
        LOG3("FU Returned from " << expression);
        auto storage = getReads(expression->e0, true);
        reads(expression, storage);   // true even in LHS
        registerUses(expression);
        lhs = save;

        hasUses->doneWatching();
        return false;
    }

    void otherExpression(const IR::Expression* expression) {
        BUG_CHECK(!lhs, "%1%: unexpected operation on LHS", expression);
        LOG3("FU Visiting [" << expression->id << "]: " << expression);
        // This expression in fact reads the result of the operation,
        // which is a temporary storage location, which we do not model
        // in the def-use analysis.
        reads(expression, LocationSet::empty);
        registerUses(expression);
    }

    void postorder(const IR::Mux* expression) override {
        otherExpression(expression);
    }

    bool preorder(const IR::ArrayIndex* expression) override {
        LOG3("FU Visiting [" << expression->id << "]: " << expression);
        if (auto cst = expression->right->to<IR::Constant>()) {
            if (lhs) {
                reads(expression, LocationSet::empty);
            } else {
                auto index = cst->asInt();
                visit(expression->left);
                auto storage = getReads(expression->left, true);
                auto result = storage->getIndex(index);
                reads(expression, result);
            }
        } else {
            // We model a write with an unknown index as a read/write
            // to the whole array.
            auto save = lhs;
            lhs = false;
            visit(expression->right);
            visit(expression->left);
            auto storage = getReads(expression->left, true);
            lhs = save;
            reads(expression, storage);
        }
        registerUses(expression);
        return false;
    }

    void postorder(const IR::Operation_Unary* expression) override {
        otherExpression(expression);
    }

    void postorder(const IR::Operation_Binary* expression) override {
        otherExpression(expression);
    }
};

class RemoveUnused : public Transform {
    const HasUses* hasUses;
    ReferenceMap*   refMap;
    TypeMap*        typeMap;

 public:
    explicit RemoveUnused(const HasUses* hasUses, ReferenceMap* refMap, TypeMap* typeMap)
                        : hasUses(hasUses), refMap(refMap), typeMap(typeMap)
    { CHECK_NULL(hasUses);  CHECK_NULL(refMap);  CHECK_NULL(typeMap); setName("RemoveUnused"); }
    const IR::Node* postorder(IR::AssignmentStatement* statement) override {
        if (!hasUses->hasUses(getOriginal())) {
            LOG3("Removing statement " << getOriginal() << " " << statement << IndentCtl::indent);
            SideEffects se(refMap, typeMap);
            (void)statement->right->apply(se);

            if (se.nodeWithSideEffect != nullptr) {
                // We expect that at this point there can't be more than 1
                // method call expression in each statement.
                BUG_CHECK(se.sideEffectCount == 1, "%1%: too many side-effect in one expression",
                          statement->right);
                BUG_CHECK(se.nodeWithSideEffect->is<IR::MethodCallExpression>(),
                          "%1%: expected method call", se.nodeWithSideEffect);
                auto mce = se.nodeWithSideEffect->to<IR::MethodCallExpression>();
                return new IR::MethodCallStatement(statement->srcInfo, mce);
            }
            // removing
            return new IR::EmptyStatement();
        }
        return statement;
    }
    const IR::Node* postorder(IR::MethodCallStatement* mcs) override {
        if (!hasUses->hasUses(getOriginal())) {
            if (SideEffects::hasSideEffect(mcs->methodCall, refMap, typeMap)) {
                return mcs;
            }
            // removing
            return new IR::EmptyStatement();
        }
        return mcs;
    }
};

// Run for each parser and control separately.
class ProcessDefUse : public PassManager {
    AllDefinitions *definitions;
    HasUses         hasUses;
 public:
    ProcessDefUse(ReferenceMap* refMap, TypeMap* typeMap) :
            definitions(new AllDefinitions(refMap, typeMap)) {
        passes.push_back(new ComputeWriteSet(definitions));
        passes.push_back(new FindUninitialized(definitions, &hasUses));
        passes.push_back(new RemoveUnused(&hasUses, refMap, typeMap));
        setName("ProcessDefUse");
    }
};
}  // namespace

const IR::Node* DoSimplifyDefUse::process(const IR::Node* node) {
    ProcessDefUse process(refMap, typeMap);
    LOG5("ProcessDefUse of:" << IndentCtl::endl << node);
    return node->apply(process);
}

}  // namespace P4
