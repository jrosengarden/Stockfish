/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2022 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>

#include "evaluate.h"
#include "movegen.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"
#include "syzygy/tbprobe.h"

using namespace std;

namespace Stockfish {

extern vector<string> setup_bench(const Position&, istream&);

// HELP system **JR** 12/10/22
void spcInput(string mesg);
char theChar;

namespace {

  // FEN string for the initial position in standard chess
  const char* StartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";


  // position() is called when the engine receives the "position" UCI command.
  // It sets up the position that is described in the given FEN string ("fen") or
  // the initial position ("startpos") and then makes the moves given in the following
  // move list ("moves").

  void position(Position& pos, istringstream& is, StateListPtr& states) {

    Move m;
    string token, fen;

    is >> token;

    if (token == "startpos")
    {
        fen = StartFEN;
        is >> token; // Consume the "moves" token, if any
    }
    else if (token == "fen")
        while (is >> token && token != "moves")
            fen += token + " ";
    else
        return;

    states = StateListPtr(new std::deque<StateInfo>(1)); // Drop the old state and create a new one
    pos.set(fen, Options["UCI_Chess960"], &states->back(), Threads.main());

    // Parse the move list, if any
    while (is >> token && (m = UCI::to_move(pos, token)) != MOVE_NONE)
    {
        states->emplace_back();
        pos.do_move(m, states->back());
    }
  }

  // trace_eval() prints the evaluation of the current position, consistent with
  // the UCI options set so far.

  void trace_eval(Position& pos) {

    StateListPtr states(new std::deque<StateInfo>(1));
    Position p;
    p.set(pos.fen(), Options["UCI_Chess960"], &states->back(), Threads.main());

    Eval::NNUE::verify();

    sync_cout << "\n" << Eval::trace(p) << sync_endl;
  }


  // setoption() is called when the engine receives the "setoption" UCI command.
  // The function updates the UCI option ("name") to the given value ("value").

  void setoption(istringstream& is) {

    string token, name, value;

    is >> token; // Consume the "name" token

    // Read the option name (can contain spaces)
    while (is >> token && token != "value")
        name += (name.empty() ? "" : " ") + token;

    // Read the option value (can contain spaces)
    while (is >> token)
        value += (value.empty() ? "" : " ") + token;

    if (Options.count(name))
        Options[name] = value;
    else
        sync_cout << "No such option: " << name << sync_endl;
  }


  // go() is called when the engine receives the "go" UCI command. The function
  // sets the thinking time and other parameters from the input string, then starts
  // with a search.

  void go(Position& pos, istringstream& is, StateListPtr& states) {

    Search::LimitsType limits;
    string token;
    bool ponderMode = false;

    limits.startTime = now(); // The search starts as early as possible

    while (is >> token)
        if (token == "searchmoves") // Needs to be the last command on the line
            while (is >> token)
                limits.searchmoves.push_back(UCI::to_move(pos, token));

        else if (token == "wtime")     is >> limits.time[WHITE];
        else if (token == "btime")     is >> limits.time[BLACK];
        else if (token == "winc")      is >> limits.inc[WHITE];
        else if (token == "binc")      is >> limits.inc[BLACK];
        else if (token == "movestogo") is >> limits.movestogo;
        else if (token == "depth")     is >> limits.depth;
        else if (token == "nodes")     is >> limits.nodes;
        else if (token == "movetime")  is >> limits.movetime;
        else if (token == "mate")      is >> limits.mate;
        else if (token == "perft")     is >> limits.perft;
        else if (token == "infinite")  limits.infinite = 1;
        else if (token == "ponder")    ponderMode = true;

    Threads.start_thinking(pos, states, limits, ponderMode);
  }


  // bench() is called when the engine receives the "bench" command.
  // Firstly, a list of UCI commands is set up according to the bench
  // parameters, then it is run one by one, printing a summary at the end.

  void bench(Position& pos, istream& args, StateListPtr& states) {

    string token;
    uint64_t num, nodes = 0, cnt = 1;

    vector<string> list = setup_bench(pos, args);
    num = count_if(list.begin(), list.end(), [](string s) { return s.find("go ") == 0 || s.find("eval") == 0; });

    TimePoint elapsed = now();

    for (const auto& cmd : list)
    {
        istringstream is(cmd);
        is >> skipws >> token;

        if (token == "go" || token == "eval")
        {
            cerr << "\nPosition: " << cnt++ << '/' << num << " (" << pos.fen() << ")" << endl;
            if (token == "go")
            {
               go(pos, is, states);
               Threads.main()->wait_for_search_finished();
               nodes += Threads.nodes_searched();
            }
            else
               trace_eval(pos);
        }
        else if (token == "setoption")  setoption(is);
        else if (token == "position")   position(pos, is, states);
        else if (token == "ucinewgame") { Search::clear(); elapsed = now(); } // Search::clear() may take a while
    }

    elapsed = now() - elapsed + 1; // Ensure positivity to avoid a 'divide by zero'

    dbg_print();

    cerr << "\n==========================="
         << "\nTotal time (ms) : " << elapsed
         << "\nNodes searched  : " << nodes
         << "\nNodes/second    : " << 1000 * nodes / elapsed << endl;
  }

  // The win rate model returns the probability of winning (in per mille units) given an
  // eval and a game ply. It fits the LTC fishtest statistics rather accurately.
  int win_rate_model(Value v, int ply) {

     // The model only captures up to 240 plies, so limit the input and then rescale
     double m = std::min(240, ply) / 64.0;

     // The coefficients of a third-order polynomial fit is based on the fishtest data
     // for two parameters that need to transform eval to the argument of a logistic
     // function.
     constexpr double as[] = {  -0.58270499,    2.68512549,   15.24638015,  344.49745382};
     constexpr double bs[] = {  -2.65734562,   15.96509799,  -20.69040836,   73.61029937 };

     // Enforce that NormalizeToPawnValue corresponds to a 50% win rate at ply 64
     static_assert(UCI::NormalizeToPawnValue == int(as[0] + as[1] + as[2] + as[3]));

     double a = (((as[0] * m + as[1]) * m + as[2]) * m) + as[3];
     double b = (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3];

     // Transform the eval to centipawns with limited range
     double x = std::clamp(double(v), -4000.0, 4000.0);

     // Return the win rate in per mille units rounded to the nearest value
     return int(0.5 + 1000 / (1 + std::exp((a - x) / b)));
  }

} // namespace


/// UCI::loop() waits for a command from the stdin, parses it and then calls the appropriate
/// function. It also intercepts an end-of-file (EOF) indication from the stdin to ensure a
/// graceful exit if the GUI dies unexpectedly. When called with some command-line arguments,
/// like running 'bench', the function returns immediately after the command is executed.
/// In addition to the UCI ones, some additional debug commands are also supported.

void UCI::loop(int argc, char* argv[]) {

  Position pos;
  string token, cmd;
  StateListPtr states(new std::deque<StateInfo>(1));

  pos.set(StartFEN, false, &states->back(), Threads.main());

  for (int i = 1; i < argc; ++i)
      cmd += std::string(argv[i]) + " ";

  do {
      if (argc == 1 && !getline(cin, cmd)) // Wait for an input or an end-of-file (EOF) indication
          cmd = "quit";

      istringstream is(cmd);

      token.clear(); // Avoid a stale if getline() returns nothing or a blank line
      is >> skipws >> token;

      if (    token == "quit"
          ||  token == "stop")
          Threads.stop = true;

      // The GUI sends 'ponderhit' to tell that the user has played the expected move.
      // So, 'ponderhit' is sent if pondering was done on the same move that the user
      // has played. The search should continue, but should also switch from pondering
      // to the normal search.
      else if (token == "ponderhit")
          Threads.main()->ponder = false; // Switch to the normal search

      else if (token == "uci")
          sync_cout << "id name " << engine_info(true)
                    << "\n"       << Options
                    << "\nuciok"  << sync_endl;

      else if (token == "setoption")  setoption(is);
      else if (token == "go")         go(pos, is, states);
      else if (token == "position")   position(pos, is, states);
      else if (token == "ucinewgame") Search::clear();
      else if (token == "isready")    sync_cout << "readyok" << sync_endl;

      // Add custom non-UCI commands, mainly for debugging purposes.
      // These commands must not be used during a search!
      else if (token == "flip")     pos.flip();
      else if (token == "bench")    bench(pos, is, states);
      else if (token == "d")        sync_cout << pos << sync_endl;
      else if (token == "eval")     trace_eval(pos);
      else if (token == "compiler") sync_cout << compiler_info() << sync_endl;
      else if (token == "export_net")
      {
          std::optional<std::string> filename;
          std::string f;
          if (is >> skipws >> f)
              filename = f;
          Eval::NNUE::save_eval(filename);
      }
 /* Commented out by **JR** 12/10/22 with replacement code below
      else if (token == "--help" || token == "help" || token == "--license" || token == "license")
          sync_cout << "\nStockfish is a powerful chess engine for playing and analyzing."
                       "\nIt is released as free software licensed under the GNU GPLv3 License."
                       "\nStockfish is normally used with a graphical user interface (GUI) and implements"
                       "\nthe Universal Chess Interface (UCI) protocol to communicate with a GUI, an API, etc."
                       "\nFor any further information, visit https://github.com/official-stockfish/Stockfish#readme"
                       "\nor read the corresponding README.md and Copying.txt files distributed along with this program.\n" << sync_endl;
      else if (!token.empty() && token[0] != '#')
          sync_cout << "Unknown command: '" << cmd << "'. Type help for more information." << sync_endl;

  } while (token != "quit" && argc == 1); // The command-line arguments are one-shot
}
*/

//Replacement code start  **JR** 12/10/22
	/// HELP system
	else if (token =="help" || token == "HELP") {
	jmppoint:
		sync_cout << "---UCI Commands---" << sync_endl;
		sync_cout << "quit		Exit Stockfish" << sync_endl;
		sync_cout << "stop		halt move search" << sync_endl;
		sync_cout << "ponderhit	start search (ponder) on same move user has played" << sync_endl;
		sync_cout << "uci		tell engine to use UCI interface (will display options)" << sync_endl;
		sync_cout << "setoption	set specific UCI option" << sync_endl;
		sync_cout << "go		start move search based on current position" << sync_endl;
		sync_cout << "help		this help screen" << sync_endl;
		sync_cout << "ucinewgame	start move search on new/different game" << sync_endl;
		sync_cout << "isready		response is 'readyok' if engine is ready and available" << sync_endl;
		sync_cout << "flip		flip sides" << sync_endl;
		sync_cout << "bench		calculate/display benchmarks for this installation of Stockfish" << sync_endl;
		sync_cout << "d		display chess board and current position of all pieces" << sync_endl;
		sync_cout << "eval		display current NNUE evaluation" << sync_endl;
		sync_cout << "compiler	display info re:compiler used for this installation of Stockfish" << sync_endl;
		sync_cout << "export_net	save current Stockfish neural network to file" << sync_endl;
		sync_cout << "position	 	set up position in fenstring or use startpos" << sync_endl;
		sync_cout << "\nAdd'l help available.  Enter in <command>/help to view. (no spaces)" << sync_endl;
		sync_cout << "\n See the following for a full UCI protocol discussion:\n    http://wbec-ridderkerk.nl/html/UCIProtocol.html" << sync_endl;
		
	}
    	else if (token == "quit/help") {	
	  	printf("[quit]\nquit the program as soon as possible\n\n");
		spcInput("Press <return> to continue --> ");
		goto jmppoint;
      }
      else if (token == "stop/help") {
	 	 printf("[stop]\nstop calculating as soon as possible, don't forget\n"
	  		"the 'bestmove' and possibly the 'ponder' token when finishing the search\n\n");
		spcInput("Press <return> to continue --> ");
		goto jmppoint;
      }
	else if (token == "ponderhit/help") {
		printf("[ponderhit]\nThe user has played the expected move. This will be sent if the engine was told\n"
			"to ponder on the same move the user has played.\n"
			"The engine should continue searching but switch from pondering to normal search.\n\n");

		spcInput("Press <return> to continue --> ");
		goto jmppoint;
	}
	else if (token == "uci/help") {
		printf("[uci]\nTell engine to use the uci (universal chess interface).\n"
			"This will be sent once, by a GUI, as a first command after program boot\n"
			"to tell the engine to switch to uci mode.\n\n"
			"After receiving the uci command the engine must identify itself with\n"
			"the 'id' command and send the 'option' commands to tell the GUI which\n"
			"engine settings the engine supports (if any).\n\n"
			"After that the engine should send 'uciok' to acknowledge the uci mode.\n"
			"If no uciok is sent within a certain time period, the engine task will \n"
			"be killed by the GUI.\n\n");

		spcInput("Press <return> to continue --> ");
		goto jmppoint;
	}
	else if (token == "setoption/help") {
		printf("[setoption]\nsetoption name <id> [value <x>]\n"
			"This is sent to the engine when the user wants to change the internal parameters\n"
			"of the engine. For the 'button' type no value is needed.\n\n"
			"One string will be sent for each parameter and this will only be sent\n"
			"when the engine is waiting.\n\n"
			"The name and value of the option in <id> should not be case sensitive and\n"
			"can include spaces.\n\n"
			"The substrings 'value' and 'name' should be avoided in <id> and <x> to allow\n"
			"unambiguous parsing, for example do not use <name> = 'draw value'.\n\n"
			"Here are some examples:\n"
			"setoption name Nullmove value true\n"
     			"setoption name Selectivity value 3\n"
	  		"setoption name Style value Risky\n"
	  		"setoption name Clear Hash\n"
	  		"setoption name NalimovPath value c:\\chess\\tb\\4;c:\\chess\\tb\\5\\n\n\n");

		spcInput("Press <return> to continue --> ");
		goto jmppoint;
	}
	else if (token == "go/help") {
		printf("[go]\nStart calculating on the current position set up with the 'position'\n"
		"command.\n\n"
		"There are a number of parameters that can follow this command and all\n"
		"will be sent in the same string.\n\n"
		"If one parameter is not sent its value should be interpreted as it would\n"
		"not influence the search.\n\n"
		"The following are the parameters and their associated values\n\n");

		printf("[go parameters]\n\n");
		printf("* searchmoves <move1> .... <movei>\n"
		"	restrict search to these moves only\n"
		"	Example: After 'position startpos' and\n"
		"		       'go infinite searchmoves e2e4 d2d4'\n"
		"	the engine should only search the two moves e2e4 and d2d4 in the\n"
		"	initial position.\n\n");

		printf("[go parameters]\n\n");
		printf("* ponder\n"
		"	start searching in pondering mode.\n"
		"	Do not exit the search in ponder mode, even if it's mate!\n\n"
		"	This means that the last move sent in in the position string\n"
		"	is the ponder move.  The engine can do what it wants to do, but\n"
		"	after a 'ponderhit' command it should execute the suggested move\n"
		"	to ponder on.\n\n"
		"	This means that the ponder move sent by the GUI can be\n"
		"	interpreted as a recommendation about which move to ponder.\n"
		"	However, if the engine decides to ponder on a different move, it\n"
		"	should not display any mainlines as they are likely to be\n"
		"	misinterpreted by the GUI because the GUI expects the engine\n"
		"	to ponder on the suggested move.\n\n");

		printf("[go parameters]\n\n");
		printf("* wtime <x>\n"
		"	white has x msec left on the clock\n"
		"* btime <x>\n"
		"	black has x msec left on the clock\n"
		"* winc <x>\n"
		"	white increment per move in mseconds if x > 0\n"
		"* binc <x>\n"
		"	black increment per move in mseconds if x > 0\n"
		"* movestogo <x>\n"
      		"	there are x moves to the next time control\n"
		"		NOTE: this will only be sent if x > 0,\n"
		"		      if you don't get this and get the\n"
		"		      wtime and btime it's sudden death\n\n"
		"* depth <x>\n"
		"	search x plies only.\n"
		"* nodes <x>\n"
	   	"	search x nodes only\n\n");

		printf("[go parameters]\n\n");
		printf("* mate <x>\n"
		"	search for a mate in x moves\n"
		"* movetime <x>\n"
		"	search exactly x mseconds\n"
		"* infinite\n"
		"	search until the 'stop' command. Do not exit the\n"
		"	search without being told so in this mode!\n\n");
		spcInput("End of go parameters. Press <return> to continue --> ");
		goto jmppoint;

	}
	else if (token == "help/help") {
	  printf("[help]\n"
		 "internal help engine usage:\n"
		 "	<uci_cmd>/help\n\n"
		 "	--will display information on that particular\n"
		 "	  UCI command\n"
		 "	     example:  help/help\n"
		 "	     will display this exact screen\n\n"
		 "external help engine usage:\n"
		 "	stockfish --help <uci_cmd optional>\n\n"
		 "	--help all by itself will display the main help screen\n"
		 "	--help followed by any UCI command will display specific\n"
		 "	  information on that particular UCI command\n"
		 "	     example:  stockfish --help help)\n"
		 "	     will display this exact screen\n\n");
		
		spcInput("Press <return> to continue --> ");
		goto jmppoint;
	}
	else if (token == "ucinewgame/help") {
		printf("[ucinewgame]\nThis is sent to the engine when the next search (started with 'position' and\n"
			"'go') will be from a different game. This can be a new game the engine should\n"
			"play or a new game it should analyse but also the next position from a testsuite\n"
			"with positions only.\n\n"
   			"If the GUI hasn't sent a 'ucinewgame' before the first 'position' command,\n"
			"the engine shouldn't expect any further ucinewgame commands as the GUI is\n"
			"probably not supporting the ucinewgame command.\n\n"
   			"So the engine should not rely on this command even though all new GUIs should\n"
			"support it.\n\n"
   			"As the engine's reaction to 'ucinewgame' can take some time the GUI should\n"
			"always send 'isready' after 'ucinewgame' to wait for the engine to finish\n"
			"its operation. The engine should respond with 'readyok'\n\n");

		spcInput("Press <return> to continue --> ");
		goto jmppoint;
	}
	else if (token == "isready/help") {
		printf("[isready]\nThis is used to synchronize the engine with the GUI.\n"
			"When the GUI has sent a command or multiple commands that can take some time\n"
			"to complete, this command can be used to wait for the engine to be ready again\n"
			"or to ping the engine to find out if it is still alive.\n\n"
			"e.g. this should be sent after setting the path to the tablebases as this\n" 
			"can take some time.\n\n"
			"This command is also required once, before the engine is asked to do any\n"
			"searching, to wait for the engine to finish initializing.\n\n"
			"This command must always be answered with 'readyok' and can be sent also when\n"
			"the engine is calculating in which case the engine should also immediately\n"
			"answer with 'readyok' without stopping the search.\n\n");

		spcInput("Press <return> to continue --> ");
		goto jmppoint;
	}
     else if(token == "flip/help") {
	  	printf("[flip]\nFlip sides in the current game\n\n");
		spcInput("Press <return> to continue --> ");
		goto jmppoint;
      }
     else if(token == "bench/help") {
	 	 printf("[bench]\ncalculate/display benchmarks for this installation of stockfish\n\n");
		spcInput("Press <return> to continue --> ");
		goto jmppoint;
      }
     else if(token == "d/help") {
	  	printf("[d]\ndisplay chess board and current position of all pieces\n\n");
		spcInput("Press <return> to continue --> ");
		goto jmppoint;
      }
     else if(token == "eval/help") {
	 	 printf("[eval]\ndisplay current NNUE evaluation\n\n");
		spcInput("Press <return> to continue --> ");
		goto jmppoint;
      }
     else if(token == "compiler/help")  {
	  	printf("[compiler]\ndisplay information about the compiler use for this installation of stockfish\n\n");
		spcInput("Press <return> to continue --> ");
		goto jmppoint;
      }
     else if(token == "export_net/help") {
	  	printf("[export_net]\nsave current stockfish neural network to file\n\n");
		spcInput("Press <return> to continue --> ");
		goto jmppoint;
      }
	else if (token == "position/help") {
		printf("[position]\nposition [fen <fenstring> | startpos ]  moves <move1> .... <movei>\n"
			"Set up the position described in fenstring on the internal board and\n"
			"play the moves on the internal chess board.\n\n"
			"If the game was played  from the start position the string 'startpos'\n"
			"must be sent\n\n"
			"Note: no 'new' command is needed. However, if this position is from\n"
			"a different game than the last position sent to the engine, the GUI\n"
			"should have sent a 'ucinewgame' in between.\n\n");

		spcInput("Press <return> to continue --> ");
		goto jmppoint;
	}													
      else if (!token.empty() && token[0] != '#')
          sync_cout << "Unknown command: " << cmd << sync_endl;

  } while (token != "quit" && argc == 1); // Command line args are one-shot
}


//Replacement code end  **JR** 12/10/22



/// UCI::value() converts a Value to a string by adhering to the UCI protocol specification:
///
/// cp <x>    The score from the engine's point of view in centipawns.
/// mate <y>  Mate in 'y' moves (not plies). If the engine is getting mated,
///           uses negative values for 'y'.

string UCI::value(Value v) {

  assert(-VALUE_INFINITE < v && v < VALUE_INFINITE);

  stringstream ss;

  if (abs(v) < VALUE_MATE_IN_MAX_PLY)
      ss << "cp " << v * 100 / NormalizeToPawnValue;
  else
      ss << "mate " << (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2;

  return ss.str();
}


/// UCI::wdl() reports the win-draw-loss (WDL) statistics given an evaluation
/// and a game ply based on the data gathered for fishtest LTC games.

string UCI::wdl(Value v, int ply) {

  stringstream ss;

  int wdl_w = win_rate_model( v, ply);
  int wdl_l = win_rate_model(-v, ply);
  int wdl_d = 1000 - wdl_w - wdl_l;
  ss << " wdl " << wdl_w << " " << wdl_d << " " << wdl_l;

  return ss.str();
}


/// UCI::square() converts a Square to a string in algebraic notation (g1, a7, etc.)

std::string UCI::square(Square s) {
  return std::string{ char('a' + file_of(s)), char('1' + rank_of(s)) };
}


/// UCI::move() converts a Move to a string in coordinate notation (g1f3, a7a8q).
/// The only special case is castling where the e1g1 notation is printed in
/// standard chess mode and in e1h1 notation it is printed in Chess960 mode.
/// Internally, all castling moves are always encoded as 'king captures rook'.

string UCI::move(Move m, bool chess960) {

  Square from = from_sq(m);
  Square to = to_sq(m);

  if (m == MOVE_NONE)
      return "(none)";

  if (m == MOVE_NULL)
      return "0000";

  if (type_of(m) == CASTLING && !chess960)
      to = make_square(to > from ? FILE_G : FILE_C, rank_of(from));

  string move = UCI::square(from) + UCI::square(to);

  if (type_of(m) == PROMOTION)
      move += " pnbrqk"[promotion_type(m)];

  return move;
}

// HELP System **JR** 12/10/22
void spcInput(string mesg) { 

	cout << mesg;
	if (scanf("%c",&theChar) != 1 ){
		fprintf( stderr, "Input Error");
	}
}


/// UCI::to_move() converts a string representing a move in coordinate notation
/// (g1f3, a7a8q) to the corresponding legal Move, if any.

Move UCI::to_move(const Position& pos, string& str) {

  if (str.length() == 5)
      str[4] = char(tolower(str[4])); // The promotion piece character must be lowercased

  for (const auto& m : MoveList<LEGAL>(pos))
      if (str == UCI::move(m, pos.is_chess960()))
          return m;

  return MOVE_NONE;
}

} // namespace Stockfish
