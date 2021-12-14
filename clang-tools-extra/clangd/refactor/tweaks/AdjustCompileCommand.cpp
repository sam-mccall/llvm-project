//===--- AdjustCompileCommand.cpp -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "Compiler.h"
#include "ParsedAST.h"
#include "refactor/Tweak.h"
#include "support/Logger.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

namespace clang {
namespace clangd {
namespace {

// Dumps the compile command
class DumpCompileCommand : public Tweak {
public:
  const char *id() const override final;
  bool prepare(const Selection &Inputs) override;
  Expected<Effect> apply(const Selection &Inputs) override;
  std::string title() const override { return "Dump compile command"; }
  llvm::StringLiteral kind() const override { return CodeAction::INFO_KIND; }

private:
  std::pair<unsigned, unsigned> CommentRange;
};

class SetCompileCommand : public Tweak {
  const char *id() const override final;
  bool prepare(const Selection &Inputs) override;
  Expected<Effect> apply(const Selection &Inputs) override;
  std::string title() const override { return "Update compile command"; }
  llvm::StringLiteral kind() const override {
    return CodeAction::REFACTOR_KIND;
  }

private:
  llvm::StringRef Comment;
  std::pair<unsigned, unsigned> CommentRange;
};

REGISTER_TWEAK(DumpCompileCommand)
REGISTER_TWEAK(SetCompileCommand)

llvm::Optional<std::pair<unsigned, unsigned>>
findEnclosingComment(const Tweak::Selection &Inputs) {
  llvm::Optional<unsigned> CommentStart;
  for (unsigned Pos = Inputs.SelectionBegin; Pos > 0; --Pos) {
    if (Inputs.Code[Pos] == '*' && Inputs.Code[Pos - 1] == '/') {
      CommentStart = Pos - 1;
      break;
    }
    if (Inputs.Code[Pos] == '\n')
      break;
  }
  if (!CommentStart)
    return llvm::None;
  if (!Inputs.Code.drop_front(*CommentStart).startswith("/* clangd flags"))
    return llvm::None;

  auto CommentEnd = Inputs.Code.find("*/", *CommentStart + 1);
  if (CommentEnd == StringRef::npos)
    return llvm::None;
  return std::pair<unsigned, unsigned>{*CommentStart, CommentEnd + 2};
}

bool DumpCompileCommand::prepare(const Selection &Inputs) {
  if (auto Range = findEnclosingComment(Inputs)) {
    CommentRange = *Range;
    return true;
  }
  return false;
}

static std::string formatCompileCommand(const tooling::CompileCommand &C) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << "/* clangd flags:";
  if (!C.Heuristic.empty())
    OS << " (" << C.Heuristic << ")";
  OS << "\n";

  OS << "[" << C.Directory << "]\n";
  for (const auto& Arg : C.CommandLine)
    OS << Arg << "\n";
  OS << "*/";

  return S;
}

Expected<Tweak::Effect> DumpCompileCommand::apply(const Selection &Inputs) {
  const auto& SM = Inputs.AST->getSourceManager();
  tooling::Replacement Edit(
      SM, SM.getComposedLoc(SM.getMainFileID(), CommentRange.first),
      CommentRange.second - CommentRange.first,
      formatCompileCommand(Inputs.AST->getCompileCommand()));
  return Tweak::Effect::mainFileEdit(Inputs.AST->getSourceManager(),
                                     tooling::Replacements(Edit));
}

bool SetCompileCommand::prepare(const Selection &Inputs) {
  if (!Inputs.CDB)
    return false;
  if (auto Range = findEnclosingComment(Inputs)) {
    CommentRange = *Range;
    Comment = Inputs.Code.slice(CommentRange.first, CommentRange.second);
    return Comment.contains('\n');
  }
  return false;
}

Expected<tooling::CompileCommand>
parseCompileCommand(const Tweak::Selection &Inputs, llvm::StringRef Comment) {
  Comment = Comment.split('\n').second;
  Comment.consume_back("*/");
  Comment = Comment.rtrim();

  tooling::CompileCommand Result = Inputs.AST->getCompileCommand();
  Result.Heuristic = "set via code action";
  auto DirCmdline = Comment.split('\n');
  llvm::StringRef Dir = DirCmdline.first;
  if (Dir.consume_front("[") && Dir.consume_back("]")) {
    Result.Directory = Dir.str();
    Comment = DirCmdline.second;
  }

  llvm::SmallVector<llvm::StringRef> CmdLine;
  Comment.split(CmdLine, "\n");
  Result.CommandLine.clear();
  for (llvm::StringRef Line : CmdLine) {
    Line = Line.trim();
    if (!Line.empty())
      Result.CommandLine.push_back(Line.str());
  }

  return Result;
}

llvm::Error validateCompileCommand(const tooling::CompileCommand &C,
                                   const ThreadsafeFS *TFS) {
  StoreDiags Diags;
  ParseInputs Inputs;
  Inputs.CompileCommand = C;
  Inputs.TFS = TFS;
  if (!buildCompilerInvocation(Inputs, Diags)) {
    std::vector<std::string> Messages;
    for (const auto& D : Diags.take())
      if (D.Severity >= DiagnosticsEngine::Error)
        Messages.push_back(D.Message);
    return error("Failed to parse compile command: [{0}]",
                 llvm::join(Messages, ", "));
  }
  return llvm::Error::success();
}

Expected<Tweak::Effect> SetCompileCommand::apply(const Selection &Inputs) {
  Expected<tooling::CompileCommand> Cmd = parseCompileCommand(Inputs, Comment);
  if (!Cmd)
    return Cmd.takeError();
  if (llvm::Error Err = validateCompileCommand(*Cmd, Inputs.TFS))
    return std::move(Err);
  std::string Path = Cmd->Filename;
  Inputs.CDB->setCompileCommand(Path, std::move(*Cmd));

  // Edit the first line of the magic comment. Serves two purposes:
  // - lets the user know something happened
  // - causes the file to be reparsed with new flags
  unsigned CommentLineLength =
      Inputs.Code.drop_front(CommentRange.first).split('\n').first.size();
  std::string ReplacementText =
      llvm::formatv("/* clangd flags: (set at {0:%H:%M:%S.%L})",
                    llvm::sys::TimePoint<>(std::chrono::system_clock::now()));
  const auto& SM = Inputs.AST->getSourceManager();
  tooling::Replacement Edit(
      SM, SM.getComposedLoc(SM.getMainFileID(), CommentRange.first),
      CommentLineLength, ReplacementText);
  return Tweak::Effect::mainFileEdit(Inputs.AST->getSourceManager(),
                                     tooling::Replacements(Edit));
}

} // namespace
} // namespace clangd
} // namespace clang

