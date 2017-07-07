// This is the main source code of the h2m autofortran tool
// envisioned by Dan Nagle, written by Sisi Liu and revised
// by Michelle Anderson at NCAR.
// It is safe to assume that all the significant comments in
// this file and in h2m.h were written by Michelle. Contact her
// at and04497@umn.edu or michellegecko@gmail.com to request
// help with the code or an explanation of an unclear comment.

// This file contains the main function as well as the main
// functions for node traversal.

#include "h2m.h"
//-----------formatter functions----------------------------------------------------------------------------------------------------

// A helper function to be used to output error line information
// If the location is invalid, it returns a message about that.
static void LineError(PresumedLoc sloc) {
  if (sloc.isValid()) {
    errs() << sloc.getFilename() << " " << sloc.getLine() << ":" << sloc.getColumn() << "\n";
  } else {
    errs() << "Invalid file location \n";
  }
}

// A helper function to be used to find lines/names which are too 
// long to be valid fortran. It returns the string which is passed
// in, regardless of the outcome of the test. The first argument
// is the string to be checked. The integer agument is the
// limit (how many characters are allowed), the boolean is whether
// or not to warn, and the Presumed location is for passing to 
// LineError if needed. 
static string CheckLength(string tocheck, int limit, bool no_warn, PresumedLoc sloc) {
  if (tocheck.length() > limit && no_warn == false) {
    errs() << "Warning: length of '" << tocheck;
    errs() << "'\n exceeds maximum. Fortran name and line lengths are limited.\n";
    LineError(sloc);
  }
  return tocheck;  // Send back the string!
}


// Fetches the fortran module name from a given filepath Filename
// IMPORTANT: ONLY call this ONCE for ANY FILE. The static 
// structure requires this, otherwise you will have all sorts of
// bizarre problems because it will think it has seen various module
// names multiple times and start appending numbers to their names.
// It keps track of modules seen in a static map and appends a number
// to the end of duplicates to create unique names. It also adds
// module_ to the front.
static string GetModuleName(string Filename, Arguments& args) {
  static std::map<string, int> repeats;
  size_t slashes = Filename.find_last_of("/\\");
  string filename = Filename.substr(slashes+1);
  size_t dot = filename.find('.');
  filename = filename.substr(0, dot);
  if (repeats.count(filename) > 0) {  // We have found a repeated module name
    int append = repeats[filename];
    repeats[filename] = repeats[filename] + 1;  // Record the new repeat in the map
    string oldfilename = filename;  // Used to report the duplicate, no other purpose
    filename = filename + "_" + std::to_string(append);  // Add _# to the end of the name
    // Note that module_ will be prepended to the name prior to returning the string.
    if (args.getSilent() == false) {
      errs() << "Warning: repeated module name module_" << oldfilename << " found, source is " << Filename;
      errs() << ". Name changed to module_" << filename << "\n";
    }
  } else {  // Record the first appearance of this module name
    repeats[filename] = 2;
  }
  filename = "module_" + filename;
  if (filename.length() > 63 && args.getSilent() == false) {
    errs() << "Warning: module name, " << filename << " too long.";
  }
  return filename;
}

// Command line options: aliases are also provided. These are all part of one category.

// Apply a custom category to all command-line options so that they are the
// only ones displayed. This avoids lots of clang options cluttering the
// help output.
static llvm::cl::OptionCategory h2mOpts("Options for the h2m translator");

// CommonOptionsParser declares HelpMessage with a description of the common
// command-line options related to the compilation database and input files.
// It's nice to have this help message in all tools.
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// A help message for this specific tool can be added afterwards.
static cl::extrahelp MoreHelp("\nSee README.txt for more information on h2m behavior.\n");

// Positional parameter: the first input parameter should be the compilation file. 
// Currently, h2m is only designed to take one file at a time.
static cl::opt<string> SourcePaths(cl::Positional, cl::cat(h2mOpts), cl::desc("<source0>"));

// Output file, option is -out or -o.
static cl::opt<string> OutputFile("out", cl::init(""), cl::cat(h2mOpts), cl::desc("Output file"));
static cl::alias OutputFile2("o", cl::desc("Alias for -out"), cl::cat(h2mOpts), cl::aliasopt(OutputFile));

// Boolean option to recursively process includes. The default is not to recursively process.
static cl::opt<bool> Recursive("recursive", cl::cat(h2mOpts), cl::desc("Include other header files recursively via USE statements"));
static cl::alias Recrusive2("r", cl::desc("Alias for -recursive"), cl::cat(h2mOpts), cl::aliasopt(Recursive));

// Boolean option to silence less critical warnings (ie warnings about statements commented out)
static cl::opt<bool> Quiet("quiet", cl::cat(h2mOpts), cl::desc("Silence warnings about lines which have been commented out"));
static cl::alias Quiet2("q", cl::desc("Alias for -quiet"), cl::cat(h2mOpts), cl::aliasopt(Quiet));

// Boolean option to silence all warnings save those that are absolutely imperative (ie output failure)
static cl::opt<bool> Silent("silent", cl::cat(h2mOpts), cl::desc("Silence all tool warnings (Clang warnings will still appear)"));
static cl::alias Silent2("s", cl::cat(h2mOpts), cl::desc("Alias for -silent"), cl::aliasopt(Silent));

// Boolean option to ignore critical clang errors that would otherwise cause termination
// during preprocessing and to keep the output file despite any other problems.
static cl::opt<bool> Optimistic("keep-going", cl::cat(h2mOpts), cl::desc("Continue processing and keep output in spite of errors"));
static cl::alias Optimistic2("k", cl::desc("Alias for -keep-going"), cl::cat(h2mOpts), cl::aliasopt(Optimistic));

// Boolean option to ignore system header files when processing recursive includes. 
// The default is to include them.
static cl::opt<bool> NoHeaders("no-system-headers", cl::cat(h2mOpts), cl::desc("Do not recursively translate system header files"));
static cl::alias NoHeaders2("n", cl::desc("Alias for -no-system-headers"), cl::cat(h2mOpts), cl::aliasopt(NoHeaders));

static cl::opt<bool> IgnoreThis("ignore-this", cl::cat(h2mOpts), cl::desc("Do not translate this file, only its included headers"));
static cl::alias IgnoreThis2("i", cl::desc("Alias for -ignore-this"), cl::cat(h2mOpts), cl::aliasopt(IgnoreThis));

// Option to specify the compiler to use to test the output. No specification means no compilation.
static cl::opt<string> Compiler("compile", cl::cat(h2mOpts), cl::desc("Command to attempt compilation of the output file"));
static cl::alias Compiler2("c", cl::desc("Alias for -compile"), cl::cat(h2mOpts), cl::aliasopt(Compiler));

// Option to send all non-system header information for this file into a single Fortran module
static cl::opt<bool> Together("together", cl::cat(h2mOpts), cl::desc("Send this entire file and all non-system includes to one module"));
static cl::alias Together2("t", cl::cat(h2mOpts), cl::desc("Alias for -together"), cl::aliasopt(Together));

// Option to transpose arrays to match Fortran dimensions (C row major
// ordering is swapped for Fortran column major ordering).
static cl::opt<bool> Transpose("array-transpose", cl::cat(h2mOpts), cl::desc("Transpose array dimensions"));
static cl::alias Transpose2("a", cl::cat(h2mOpts), cl::desc("Alias for array-transpose"), cl::aliasopt(Transpose));

static cl::opt<bool> Autobind("auto-bind", cl::cat(h2mOpts), cl::desc("Use BIND(C, name=...) to handle illegal names"));
static cl::alias Autobind2("b", cl::cat(h2mOpts), cl::desc("Alias for auto-bind"), cl::aliasopt(Autobind));

// These are the argunents for the clang compiler driver.
static cl::opt<string> other(cl::ConsumeAfter, cl::desc("Front end arguments"));


//-----------AST visit functions----------------------------------------------------------------------------------------------------

// This function does the work of determining what the node currently under traversal
// is and creating the proper object-translation object to handle it.
bool TraverseNodeVisitor::TraverseDecl(Decl *d) {
  if (isa<TranslationUnitDecl> (d)) {
    // tranlastion unit decl is the top node of all AST, ignore the inner structure of tud for now
    RecursiveASTVisitor<TraverseNodeVisitor>::TraverseDecl(d);

  } else if (isa<FunctionDecl> (d)) {
    // Keep included header files out of the mix by checking the location
    // Includes become part of the AST. However, there shouldn't be any
    // problem with potentially invalid locations during the check. The
    // Function checks for that.
    // Note that, if the option Together is specified, the entire AST (save
    // for system headers which are checked for elsewhere) is sent to the
    // same file due to the || statement
    if (TheRewriter.getSourceMgr().isInMainFile(d->getLocation()) ||
        args.getTogether() == true) {
      FunctionDeclFormatter fdf(cast<FunctionDecl> (d), TheRewriter, args);
      allFunctionDecls += fdf.getFortranFunctDeclASString();
    }
    
    // args.getOutput().os() << "INTERFACE\n" 
    // << fdf.getFortranFunctDeclASString()
    // << "END INTERFACE\n";      
  } else if (isa<TypedefDecl> (d)) {
    // Keep included header files out of the mix by checking the location
    if (TheRewriter.getSourceMgr().isInMainFile(d->getLocation()) ||
        args.getTogether() == true) {
      TypedefDecl *tdd = cast<TypedefDecl> (d);
      TypedefDeclFormater tdf(tdd, TheRewriter, args);
      args.getOutput().os() << tdf.getFortranTypedefDeclASString();
    }

  } else if (isa<RecordDecl> (d)) {
    // Keep included header files out of the mix by checking the location
    if (TheRewriter.getSourceMgr().isInMainFile(d->getLocation()) ||
        args.getTogether() == true) {
      RecordDecl *rd = cast<RecordDecl> (d);
      RecordDeclFormatter rdf(rd, TheRewriter, args);
      args.getOutput().os() << rdf.getFortranStructASString();
    }

  } else if (isa<VarDecl> (d)) {
    // Keep included header files out of the mix by checking the location
    if (TheRewriter.getSourceMgr().isInMainFile(d->getLocation()) ||
        args.getTogether() == true) {
      VarDecl *varDecl = cast<VarDecl> (d);
      VarDeclFormatter vdf(varDecl, TheRewriter, args);
      args.getOutput().os() << vdf.getFortranVarDeclASString();
    } 

  } else if (isa<EnumDecl> (d)) {
    // Keep included header files out of the mix by checking the location
    if (TheRewriter.getSourceMgr().isInMainFile(d->getLocation()) ||
        args.getTogether() == true) {
      EnumDeclFormatter edf(cast<EnumDecl> (d), TheRewriter, args);
      args.getOutput().os() << edf.getFortranEnumASString();
    }
  } else {

    // Keep included header files out of the mix by checking the location
    if (TheRewriter.getSourceMgr().isInMainFile(d->getLocation()) ||
        args.getTogether() == true) {
      args.getOutput().os() << "!found other type of declaration \n";
      d->dump();
      RecursiveASTVisitor<TraverseNodeVisitor>::TraverseDecl(d);
    }
  }
    // comment out because function declaration doesn't need to be traversed.
    // RecursiveASTVisitor<TraverseNodeVisitor>::TraverseDecl(d); // Forward to base class

    return true; // Return false to stop the AST analyzing

};


// Currently, there are no attempts made to traverse and translate statements into 
// Fortran. This method simply comments out statements and warns about them if
// necessary.
bool TraverseNodeVisitor::TraverseStmt(Stmt *x) {
  string stmtText;
  string stmtSrc = Lexer::getSourceText(CharSourceRange::getTokenRange(x->getLocStart(), x->getLocEnd()), TheRewriter.getSourceMgr(), LangOptions(), 0);
  // comment out stmtText
  std::istringstream in(stmtSrc);
  for (std::string line; std::getline(in, line);) {
    // Output warnings about commented out statements only if a loud run is in progress.
    if (args.getQuiet() == false && args.getSilent() == false) {
      errs() << "Warning: statement " << stmtText << " commented out.\n";
      LineError(TheRewriter.getSourceMgr().getPresumedLoc(x->getLocStart()));
    }
    stmtText += "! " + line + "\n";
  }
  args.getOutput().os() << stmtText;

  RecursiveASTVisitor<TraverseNodeVisitor>::TraverseStmt(x);
  return true;
};
// Currently, there are no attempts made to traverse and translate types 
// into Fotran. This method simply comments out the declaration.
bool TraverseNodeVisitor::TraverseType(QualType x) {
  string qt_string = "!" + x.getAsString();
  args.getOutput().os() << qt_string;
  if (args.getQuiet() == false && args.getSilent() == false) { 
    errs() << "Warning: type " << qt_string << " commented out.\n";
  }
  RecursiveASTVisitor<TraverseNodeVisitor>::TraverseType(x);
  return true;
};

//---------------------------Main Program Class Functions---------
void TraverseMacros::MacroDefined (const Token &MacroNameTok, const MacroDirective *MD) {
    MacroFormatter mf(MacroNameTok, MD, ci, args);
    args.getOutput().os() << mf.getFortranMacroASString();
}

// HandlTranslationUnit is the overarching entry into the clang ast which is
// called to begin the traversal.
void TraverseNodeConsumer::HandleTranslationUnit(clang::ASTContext &Context) {
// Traversing the translation unit decl via a RecursiveASTVisitor
// will visit all nodes in the AST.

  Visitor.TraverseDecl(Context.getTranslationUnitDecl());

  // wrap all func decls in a single interface
  if (!Visitor.allFunctionDecls.empty()) {
    args.getOutput().os() << "INTERFACE\n" 
    << Visitor.allFunctionDecls
    << "END INTERFACE\n";   
  }
}

// Executed when each source begins. This allows the boiler plate required for each
// module to be added to the file.
bool TraverseNodeAction::BeginSourceFileAction(CompilerInstance &ci, StringRef Filename)
{
  fullPathFileName = Filename;
  // We have to pass this back out to keep track of repeated module names
  args.setModuleName(GetModuleName(Filename, args));

  // initalize Module and imports
  string beginSourceModule;
  beginSourceModule = "MODULE " + args.getModuleName() + "\n";
  beginSourceModule += "USE, INTRINSIC :: iso_c_binding\n";
  beginSourceModule += use_modules;
  beginSourceModule += "implicit none\n";
  args.getOutput().os() << beginSourceModule;

  Preprocessor &pp = ci.getPreprocessor();
  pp.addPPCallbacks(llvm::make_unique<TraverseMacros>(ci, args));
  return true;
}

// Executed when a source file is finished. This allows the boiler plate required for the end of
// a fotran module to be added to the file.
void TraverseNodeAction::EndSourceFileAction() {
    //Now emit the rewritten buffer.
    //TheRewriter.getEditBuffer(TheRewriter.getSourceMgr().getMainFileID()).write(args.getOutput().os());

    string endSourceModle;
    endSourceModle = "END MODULE " + args.getModuleName() + "\n";
    args.getOutput().os() << endSourceModle;
  }

int main(int argc, const char **argv) {
  if (argc > 1) {
    // Parse the command line options and create a new database to hold the Clang compilation
    // options.
    cl::HideUnrelatedOptions(h2mOpts); // This hides all the annoying clang options in help output
    cl::ParseCommandLineOptions(argc, argv, "h2m Autofortran Tool\n");
    std::unique_ptr<CompilationDatabase> Compilations;
    SmallString<256> PathBuf;
    sys::fs::current_path(PathBuf);
    Compilations.reset(new FixedCompilationDatabase(Twine(PathBuf), other));


    // Checks for illegal options. Give warnings, and errors.
    if (IgnoreThis == true && Recursive == false) {  // Don't translate this, and no recursion requested
      errs() << "Error: incompatible options, skip given file and no recursion (-i without -r)";
      errs() << "Either specify a recursive translation or remove the option to skip the first file.";
      return(1);
    // If all the main file's AST (including headers courtesty of the preprocessor) is sent to one
    // module and a recursive option is also specified, things defined in the headers will be
    // defined multiple times over several recursively translated modules.
    } else if (Together == true && Recursive == true) {
      errs() << "Warning: request for all local includes to be sent to a single file accompanied\n";
      errs() << "by recursive translation (-t and -r) may result in multiple declaration errors.\n";
    }
    // Determine file to open and initialize it. Wrtie to stdout if no file is given.
    string filename;
    std::error_code error;
    if (OutputFile.size()) {
      filename = OutputFile;
    } else {
      filename = "-";
    }
    // The file is opened in text mode
    llvm::tool_output_file OutputFile(filename, error, llvm::sys::fs::F_Text);
    if (error) {  // Error opening file
      errs() << "Error opening output file: " << filename << error.message() << "\n";
      return(1);  // We can't possibly keep going if the file can't be opened.
    }
    if (Optimistic == true) {  // Keep all output inspite of errors
      OutputFile.keep();
    }
    // Create an object to pass around arguments
    Arguments args(Quiet, Silent, OutputFile, NoHeaders, Together, Transpose, Autobind);


    // Create a new clang tool to be used to run the frontend actions
    ClangTool Tool(*Compilations, SourcePaths);
    int tool_errors = 0;  // No errors have occurred running the tool yet


    // Write some initial text into the file, just boilerplate stuff.
    OutputFile.os() << "! The following Fortran code was generated by the h2m-Autofortran ";
    OutputFile.os() << "Tool.\n! See the h2m README file for credits and help information.\n\n";

    // Follow the preprocessor's inclusions to generate a recursive 
    // order of hearders to be translated and linked by "USE" statements
    if (Recursive) {
      std::set<string> seenfiles;
      std::stack<string> stackfiles;
      // CHS means "CreateHeaderStack." The seenfiles is a set used to keep track of what 
      // has been seen, and the stackfiles is used to keep track of the order to include them.
      CHSFrontendActionFactory CHSFactory(seenfiles, stackfiles, args);
      int initerrs = Tool.run(&CHSFactory);  // Run the first action to follow inclusions
      // If the attempt to find the needed order to translate the headers fails,
      // this effort is probably doomed.
      if (initerrs != 0) {
        errs() << "Error during preprocessor-tracing tool run, errno ." << initerrs << "\n";
        if (Optimistic == false) {  // Exit unless told to keep going
          errs() << "A non-recursive run may succeed.\n";
          errs() << "Alternately, enable optimistic mode (-keep-going or -k) to continue despite errors.\n";
          return(initerrs);
        } else if (stackfiles.empty() == true) {  // Whatever happend is not recoverable.
          errs() << "Unrecoverable initialization error. No files recorded to translate.\n";
          return(initerrs);
        } else {
          errs() << "Optimistic run continuing.\n";
        }
      }

      // Dig through the created stack of header files we have seen, as prepared by
      // the first Clang tool which tracks the preprocessor. Translate them each in turn.
      string modules_list;
      while (stackfiles.empty() == false) {
        string headerfile = stackfiles.top();
        stackfiles.pop(); 

        if (stackfiles.empty() && IgnoreThis == true) {  // We were directed to skip the first file
          break;  // Leave the while loop. This is cleaner than another level of indentation.
        }

        ClangTool stacktool(*Compilations, headerfile);  // Create a tool to run on each file in turn
        TNAFrontendActionFactory factory(modules_list, args);
        // modules_list is the growing string of previously translated modules this module may depend on
        // Run the translation tool.
        tool_errors = stacktool.run(&factory);

        if (tool_errors != 0) {  // Tool error occurred
          if (Silent == false) {  // Do not report the error if the run is silent.
            errs() << "Translation error occured on " << headerfile;
            errs() <<  ". Output may be corrupted or missing.\n";
            errs() << "\n\n\n\n";  // Put four lines between files to help keep track of errors
          }
          // Comment out the use statement becuase the module may be corrupt.
          modules_list += "!USE " + args.getModuleName() + "\n";
          OutputFile.os()  << "! Warning: Translation Error Occurred on this module\n";
        } else {  // Successful run, no errors
          // Add USE statement to be included in future modules
          modules_list += "USE " + args.getModuleName() + "\n";
          if (Silent == false) {  // Don't clutter the screen if the run is silent
            errs() << "Successfully processed " << headerfile << "\n";
            errs() << "\n\n\n\n";  // Put four lines between files to help keep track of errors
          }
        }
        args.setModuleName("");  // For safety, unset the module name passed out of Arguments

        args.getOutput().os() << "\n\n";  // Put two lines inbetween modules, even on a trans. failure
      }  // End looking through the stack and processing all headers (including the original).

    } else {  // No recursion, just run the tool on the first input file. No module list string is needed.
      TNAFrontendActionFactory factory("", args);
      tool_errors = Tool.run(&factory);
    }  // End processing of the translation

    // Note that the output has already been kept if this is an optimistic run. It doesn't hurt to keep twice.
    if (!tool_errors) {  // If the last run of the tool was not successful, the output may be garbage.
      OutputFile.keep();
    }

    // A compiler post-process has been specified and there is an actual output file,
    // prepare and run the compiler.
    if (Compiler.size() && filename.compare("-") != 0) {
      OutputFile.os().flush();  // Attempt to flush the stream to avoid a partial read by the compiler
      if (system(NULL) == true) {  // A command interpreter is available
        string command = Compiler + " " + filename;
        int success = system(command.c_str());
	if (Silent == false) {  // Notify if the run is noisy
	  if (success == 0) {  // Successful compilation.
	    errs() << "Successful compilation of " << filename << " with " << Compiler << "\n";
	  } else {  // Inform of the error and give the error number
	    errs() << "Unsuccessful compilation of " << filename << " with ";
	    errs() << Compiler << ". Error: " << success << "\n";
	  } 
        }
      } else {  // Cannot run using system (fork might succeed but is very error prone).
        errs() << "Error: No command interpreter available to run system process " << Compiler << "\n";
      }
    // We were asked to run the compiler, but there is no output file, report an error.
    } else if (Compiler.size() && filename.compare("-") == 0) {
      errs() << "Error: unable to attempt compilation on standard output.\n";
    }
    return(tool_errors);
  }

  errs() << "At least one argument (header to process) must be provided.\n";
  errs() << "Run 'h2m -help' for usage details.\n";
  return(1);  // Someone did not give any arguments
};
