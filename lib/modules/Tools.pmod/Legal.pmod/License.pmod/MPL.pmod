#pike __REAL_VERSION__

// $Id: MPL.pmod,v 1.1 2002/05/31 16:20:18 nilsson Exp $

private constant text = #string "mpl.txt";

string get_text() {
  return text;
}

string get_xml() {
  return "<pre>\n" + get_text() + "</pre>\n";
}
