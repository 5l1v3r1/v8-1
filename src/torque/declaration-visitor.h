// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_TORQUE_DECLARATION_VISITOR_H_
#define V8_TORQUE_DECLARATION_VISITOR_H_

#include <set>
#include <string>

#include "src/torque/file-visitor.h"
#include "src/torque/global-context.h"
#include "src/torque/scope.h"
#include "src/torque/types.h"
#include "src/torque/utils.h"

namespace v8 {
namespace internal {
namespace torque {

class DeclarationVisitor : public FileVisitor {
 public:
  explicit DeclarationVisitor(GlobalContext& global_context)
      : FileVisitor(global_context) {
    GetTypeOracle().RegisterTypeImpl(EXCEPTION_TYPE_STRING, "Label*", nullptr);
  }

  void Visit(Ast* ast) { Visit(ast->GetDefaultModule()); }

  void Visit(Expression* expr);
  void Visit(Statement* stmt);
  void Visit(Declaration* decl);

  void Visit(ModuleDeclaration* decl) {
    Module* saved_module = module_;
    module_ = decl->GetModule();
    Scope::Activator activator(module_->scope());
    for (Declaration* child : decl->declarations) Visit(child);
    module_ = saved_module;
  }
  void Visit(DefaultModuleDeclaration* decl) {
    decl->SetModule(global_context_.GetDefaultModule());
    Visit(base::implicit_cast<ModuleDeclaration*>(decl));
  }
  void Visit(ExplicitModuleDeclaration* decl) {
    decl->SetModule(global_context_.GetModule(decl->name));
    Visit(base::implicit_cast<ModuleDeclaration*>(decl));
  }

  void Visit(IdentifierExpression* expr) {}
  void Visit(NumberLiteralExpression* expr) {}
  void Visit(StringLiteralExpression* expr) {}
  void Visit(CallExpression* expr) {
    for (Expression* arg : expr->arguments) Visit(arg);
  }
  void Visit(ElementAccessExpression* expr) {
    Visit(expr->array);
    Visit(expr->index);
  }
  void Visit(FieldAccessExpression* expr) { Visit(expr->object); }
  void Visit(CastExpression* expr) { Visit(expr->value); }
  void Visit(ConvertExpression* expr) { Visit(expr->value); }
  void Visit(BlockStatement* expr) {
    Scope::Activator s(global_context_, expr);
    for (Statement* stmt : expr->statements) Visit(stmt);
  }
  void Visit(ExpressionStatement* stmt) { Visit(stmt->expression); }
  void Visit(TailCallStatement* stmt) { Visit(stmt->call); }

  void Visit(TypeDeclaration* decl) {
    std::string generates_class_name =
        decl->generates ? *decl->generates : decl->name;
    GetTypeOracle().RegisterTypeImpl(decl->name, generates_class_name,
                                     decl->extends ? &*decl->extends : nullptr);
  }

  void Visit(ExternalBuiltinDeclaration* decl) {
    if (global_context_.verbose()) {
      std::cout << "found declaration of external builtin " << decl->name
                << " with signature ";
    }

    Signature signature =
        MakeSignature(decl->parameters, decl->return_type, {});

    if (signature.parameter_types.types.size() == 0 ||
        !signature.parameter_types.types[0].Is(CONTEXT_TYPE_STRING)) {
      std::stringstream stream;
      stream << "first parameter to builtin " << decl->name
             << " is not a context but should be at "
             << PositionAsString(decl->pos);
      ReportError(stream.str());
    }
    const bool javascript = decl->javascript_linkage;
    const bool varargs = decl->parameters.has_varargs;
    if (varargs && !javascript) {
      std::stringstream stream;
      stream << "builtin " << decl->name
             << " with rest parameters must be a JavaScript builtin at "
             << PositionAsString(decl->pos);
      ReportError(stream.str());
    }
    if (javascript) {
      if (signature.types().size() < 2 ||
          !signature.types()[1].Is(OBJECT_TYPE_STRING)) {
        std::stringstream stream;
        stream << "second parameter to javascript builtin " << decl->name
               << " is not a receiver type but should be at "
               << PositionAsString(decl->pos);
        ReportError(stream.str());
      }
    }
    Builtin::Kind kind = !javascript ? Builtin::kStub
                                     : varargs ? Builtin::kVarArgsJavaScript
                                               : Builtin::kFixedArgsJavaScript;
    TopScope()->DeclareBuiltin(decl->pos, decl->name, kind, nullptr, signature);
  }

  void Visit(ExternalRuntimeDeclaration* decl) {
    if (global_context_.verbose()) {
      std::cout << "found declaration of external runtime " << decl->name
                << " with signature ";
    }
    Type return_type = GetType(decl->return_type);
    TypeVector parameter_types = GetTypeVector(decl->parameters.types);
    if (parameter_types.size() == 0 ||
        !parameter_types[0].Is(CONTEXT_TYPE_STRING)) {
      std::stringstream stream;
      stream << "first parameter to runtime " << decl->name
             << " is not a context but should be at "
             << PositionAsString(decl->pos);
      ReportError(stream.str());
    }

    Signature signature{{},
                        {parameter_types, decl->parameters.has_varargs},
                        return_type,
                        {},
                        {}};
    TopScope()->DeclareRuntime(decl->pos, decl->name, nullptr,
                               std::move(signature));
  }

  void Visit(ExternalMacroDeclaration* decl) {
    if (global_context_.verbose()) {
      std::cout << "found declaration of external macro " << decl->name
                << " with signature ";
    }

    Signature signature =
        MakeSignature(decl->parameters, decl->return_type, decl->labels);

    TopScope()->DeclareMacro(decl->pos, decl->name, nullptr, signature);
    if (decl->op) {
      OperationHandler handler(
          {decl->name, signature.parameter_types, signature.return_type});
      auto i = global_context_.op_handlers_.find(*decl->op);
      if (i == global_context_.op_handlers_.end()) {
        global_context_.op_handlers_[*decl->op] =
            std::vector<OperationHandler>();
        i = global_context_.op_handlers_.find(*decl->op);
      }
      i->second.push_back(handler);
    }

    if (decl->implicit) {
      if (!decl->op || *decl->op != "convert<>") {
        std::stringstream s;
        s << "implicit can only be used with cast<> operator at "
          << PositionAsString(decl->pos) << "\n";
        ReportError(s.str());
      }

      const TypeVector& parameter_types = signature.parameter_types.types;
      if (parameter_types.size() != 1 || signature.parameter_types.var_args) {
        std::stringstream s;
        s << "implicit cast operators doesn't only have a single parameter at "
          << PositionAsString(decl->pos) << "\n";
        ReportError(s.str());
      }
      GetTypeOracle().RegisterImplicitConversion(signature.return_type,
                                                 parameter_types[0]);
    }
  }

  void Visit(BuiltinDeclaration* decl);
  void Visit(MacroDeclaration* decl);
  void Visit(ReturnStatement* stmt);

  void Visit(VarDeclarationStatement* stmt) {
    std::string variable_name = stmt->name;
    Type type = GetType(stmt->type);
    TopScope()->DeclareVariable(stmt->pos, variable_name, type);
    if (global_context_.verbose()) {
      std::cout << "declared variable " << variable_name << " with type "
                << type << "\n";
    }
    if (stmt->initializer) {
      Visit(*stmt->initializer);
      if (global_context_.verbose()) {
        std::cout << "variable has initialization expression at "
                  << PositionAsString(stmt->pos) << "\n";
      }
    }
  }

  void Visit(ConstDeclaration* decl) {
    TopScope()->DeclareConstant(decl->pos, decl->name, GetType(decl->type),
                                decl->literal);
  }

  void Visit(LogicalOrExpression* expr) {
    {
      Scope::Activator s(global_context_, expr->left);
      TopScope()->DeclareLabel(expr->pos, kFalseLabelName);
      Visit(expr->left);
    }
    Visit(expr->right);
  }

  void Visit(LogicalAndExpression* expr) {
    {
      Scope::Activator s(global_context_, expr->left);
      TopScope()->DeclareLabel(expr->pos, kTrueLabelName);
      Visit(expr->left);
    }
    Visit(expr->right);
  }

  void DeclareExpressionForBranch(Expression* node) {
    Scope::Activator s(global_context_, node);
    // Conditional expressions can either explicitly return a bit
    // type, or they can be backed by macros that don't return but
    // take a true and false label. By declaring the labels before
    // visiting the conditional expression, those label-based
    // macro conditionals will be able to find them through normal
    // label lookups.
    TopScope()->DeclareLabel(node->pos, kTrueLabelName);
    TopScope()->DeclareLabel(node->pos, kFalseLabelName);
    Visit(node);
  }

  void Visit(ConditionalExpression* expr) {
    DeclareExpressionForBranch(expr->condition);
    PushControlSplit();
    Visit(expr->if_true);
    Visit(expr->if_false);
    auto changed_vars = PopControlSplit();
    global_context_.AddControlSplitChangedVariables(expr, changed_vars);
  }

  void Visit(IfStatement* stmt) {
    PushControlSplit();
    DeclareExpressionForBranch(stmt->condition);
    Visit(stmt->if_true);
    if (stmt->if_false) Visit(*stmt->if_false);
    auto changed_vars = PopControlSplit();
    global_context_.AddControlSplitChangedVariables(stmt, changed_vars);
  }

  void Visit(WhileStatement* stmt) {
    Scope::Activator s(global_context_, stmt);
    DeclareExpressionForBranch(stmt->condition);
    PushControlSplit();
    Visit(stmt->body);
    auto changed_vars = PopControlSplit();
    global_context_.AddControlSplitChangedVariables(stmt, changed_vars);
  }

  void Visit(ForOfLoopStatement* stmt);

  void Visit(AssignmentExpression* expr) {
    MarkLocationModified(expr->location);
    Visit(expr->location);
    Visit(expr->value);
  }

  void Visit(BreakStatement* stmt) {}
  void Visit(ContinueStatement* stmt) {}
  void Visit(GotoStatement* expr) {}

  void Visit(ForLoopStatement* stmt) {
    Scope::Activator s(global_context_, stmt);
    if (stmt->var_declaration) Visit(*stmt->var_declaration);
    PushControlSplit();
    DeclareExpressionForBranch(stmt->test);
    Visit(stmt->body);
    Visit(stmt->action);
    auto changed_vars = PopControlSplit();
    global_context_.AddControlSplitChangedVariables(stmt, changed_vars);
  }

  void Visit(IncrementDecrementExpression* expr) {
    MarkLocationModified(expr->location);
    Visit(expr->location);
  }

  void Visit(TryCatchStatement* stmt);

  void GenerateHeader(std::string& file_name) {
    std::stringstream new_contents_stream;
    new_contents_stream
        << "#ifndef V8_BUILTINS_BUILTIN_DEFINITIONS_FROM_DSL_H_\n"
           "#define V8_BUILTINS_BUILTIN_DEFINITIONS_FROM_DSL_H_\n"
           "\n"
           "#define BUILTIN_LIST_FROM_DSL(CPP, API, TFJ, TFC, TFS, TFH, ASM) "
           "\\\n";
    for (auto builtin : defined_builtins_) {
      int firstParameterIndex = 1;
      bool declareParameters = true;
      if (builtin->IsStub()) {
        new_contents_stream << "TFS(" << builtin->name();
      } else {
        new_contents_stream << "TFJ(" << builtin->name();
        if (builtin->IsVarArgsJavaScript()) {
          new_contents_stream
              << ", SharedFunctionInfo::kDontAdaptArgumentsSentinel";
          declareParameters = false;
        } else {
          assert(builtin->IsFixedArgsJavaScript());
          // FixedArg javascript builtins need to offer the parameter
          // count.
          assert(builtin->parameter_names().size() >= 2);
          new_contents_stream << ", "
                              << (builtin->parameter_names().size() - 2);
          // And the receiver is implicitly declared.
          firstParameterIndex = 2;
        }
      }
      if (declareParameters) {
        int index = 0;
        for (auto parameter : builtin->parameter_names()) {
          if (index >= firstParameterIndex) {
            new_contents_stream << ", k" << CamelifyString(parameter);
          }
          index++;
        }
      }
      new_contents_stream << ") \\\n";
    }
    new_contents_stream
        << "\n"
           "#endif  // V8_BUILTINS_BUILTIN_DEFINITIONS_FROM_DSL_H_\n";

    std::string new_contents(new_contents_stream.str());
    ReplaceFileContentsIfDifferent(file_name, new_contents);
  }

 private:
  struct LiveAndChanged {
    std::set<const Variable*> live;
    std::set<const Variable*> changed;
  };

  void PushControlSplit() {
    LiveAndChanged live_and_changed;
    live_and_changed.live = global_context_.GetLiveTypeVariables();
    live_and_changed_variables_.push_back(live_and_changed);
  }

  std::set<const Variable*> PopControlSplit() {
    auto result = live_and_changed_variables_.back().changed;
    live_and_changed_variables_.pop_back();
    return result;
  }

  void MarkLocationModified(Expression* location) {
    if (IdentifierExpression* id = IdentifierExpression::cast(location)) {
      const Value* value = LookupValue(id->pos, id->name);
      if (value->IsVariable()) {
        const Variable* variable = Variable::cast(value);
        bool was_live = MarkVariableModified(variable);
        if (was_live && global_context_.verbose()) {
          std::cout << *variable << " was modified in control split at "
                    << PositionAsString(id->pos) << "\n";
        }
      }
    }
  }

  bool MarkVariableModified(const Variable* variable) {
    auto e = live_and_changed_variables_.rend();
    auto c = live_and_changed_variables_.rbegin();
    bool was_live_in_preceeding_split = false;
    while (c != e) {
      if (c->live.find(variable) != c->live.end()) {
        c->changed.insert(variable);
        was_live_in_preceeding_split = true;
      }
      c++;
    }
    return was_live_in_preceeding_split;
  }

  void DeclareParameterList(SourcePosition pos, const Signature& signature) {
    auto name_iterator = signature.parameter_names.begin();
    for (auto t : signature.types()) {
      const std::string& name(*name_iterator++);
      TopScope()->DeclareParameter(pos, name,
                                   GetParameterVariableFromName(name), t);
    }
    size_t i = 0;
    for (auto definition : signature.label_defintions) {
      TopScope()->DeclareLabel(pos, definition.name, signature.labels[i++]);
    }
  }

  std::vector<Builtin*> defined_builtins_;
  std::vector<LiveAndChanged> live_and_changed_variables_;
};

}  // namespace torque
}  // namespace internal
}  // namespace v8

#endif  // V8_TORQUE_DECLARATION_VISITOR_H_
