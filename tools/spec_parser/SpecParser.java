// Copyright (c) 2017, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import org.antlr.runtime.*;

/// Class for `main` which will parse files given as command line arguments.
public class SpecParser {
  static boolean verbose = false;

  public static void main(String[] args) throws Exception {
    if (args.length == 0) {
      System.err.println("Expected a file path as argument.");
      System.exit(1);
    }
    for (int i = 0; i < args.length; i++) {
      String filePath = args[i];
      if (filePath.equals("--verbose")) {
        verbose = true;
        continue;
      }
      CharStream charStream = new ANTLRFileStream(filePath);
      DartLexer lexer = new DartLexer(charStream);
      CommonTokenStream tokens = new CommonTokenStream(lexer);
      DartParser parser = new DartParser(tokens);
      DartParser.filePath = filePath;
      DartParser.filePathHasBeenPrinted = false;
      if (verbose) System.err.println(">>> Parsing file: " + filePath);
      parser.libraryDefinition();
    }
  }
}
