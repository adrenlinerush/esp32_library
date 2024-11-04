#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <sqlite3.h>
#include <SPI.h>
#include <FS.h>
#include "SD.h"
#include <ESPmDNS.h>
#include <map>
#include <time.h>

std::map<String, String> config;
std::map<String, String> sessions;

IPAddress allowedHost;

const int RESULTS_PER_PAGE = 15;
char* errMsg;
sqlite3 *db;
int rc;
sqlite3_stmt *res;
int rec_count = 0;
const char *tail;
char current_db[255];
const char* db_filename = "/sd/library.db";

WebServer server(80);

const char* headersToCollect[] = {"Cookie"};

IPAddress parseIPAddress(const String &ipStr) {
    int parts[4] = {0};
    int partIndex = 0;

    int start = 0;
    for (int i = 0; i < ipStr.length(); i++) {
        if (ipStr.charAt(i) == '.' || i == ipStr.length() - 1) {
            if (i == ipStr.length() - 1) i++;
            parts[partIndex++] = ipStr.substring(start, i).toInt();
            start = i + 1;
        }
    }

    return IPAddress(parts[0], parts[1], parts[2], parts[3]);
}

void readConfig(fs::FS &fs){
  Serial.println("Openning config file...");
  File configFile = fs.open("/config", FILE_READ);
  if (configFile) {
    Serial.println("Reading config ...");
    while (configFile.available()) {
      String line = configFile.readStringUntil('\n');
      unsigned eq = line.indexOf("=");
      String key = line.substring(0,eq);
      //Serial.printf("Found key: %s\n", key);
      String value = line.substring(eq+1);
      config[key]=value;
      if (key == "proxy_host") {
        allowedHost = parseIPAddress(value);
      } 
    }
    configFile.close(); 
  } 
  else {
    Serial.println("error opening config");
  }
}


String generateSessionToken() {
    return String(random(100000, 999999));
}

bool is_admin() {
  IPAddress clientIP = server.client().remoteIP();
  String token = server.header("Cookie");
  token = token.substring(token.indexOf("session=") + 8);
  bool has_token = sessions.count(token) > 0;
  if (has_token && clientIP == allowedHost) {
    return true;
  } else {
    return false;
  }
}


String renderHeader(){
  String currentRoute = server.uri();
  String html = "<html><head><title>Adrenlinerush Library Catalog</title>";
  html += "<style> body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; font-size: large; Color: #000088; }";
  html += "a{display:block;float:left;padding:10px 15px;background:#aaa;border:1px solid #777;border-bottom:none;border-radius:4px 4px 0 0;margin-right:1px;color:#fff;text-decoration:none} </style>";
  html += "</head> <body> <h2>Adrenlinerush Library Catalog</h2>";
  html += "<p><img src=\"/img?name=adrenaline.png\"></p>";
  html += "<a href=\"/\">Home</a>";
  html += "<a href=\"/search\">Search</a>";
  if (currentRoute == "/details" && is_admin()) {
    String id = server.arg("id");
    html += "<a href=\"/edit?id=" + id +"\">Edit</a>";
  }
  if (is_admin()) {
    html += "<a href=\"/add\">Add</a>";
    html += "<a href=\"/logout\">Log Out</a>";
  } else {
    html += "<a href=\"/login\">Log In</a>";
  }
  html += "<br><br><br>";
  return html;
}

void renderAdd(){
  if (is_admin()) {
    String html = renderHeader();
    html += "<h2>Add a New Book</h2>";
    html += "<form action=\"/add\" method=\"POST\">";
    html += "Title: <input type=\"text\" name=\"title\"><br>";
    html += "Author: <input type=\"text\" name=\"author\"><br>";
    html += "ISBN: <input type=\"text\" name=\"isbn\"><br>";
    html += "Location: <input type=\"text\" name=\"location\"><br>";
    html += "Keywords: <input type=\"text\" name=\"keywords\"><br>";
    html += "Synopsis: <textarea name=\"synopsis\"></textarea><br>";
    html += "<input type=\"submit\" value=\"Add Book\">";
    html += "</form>";
    html += "</body></html>";
    server.send (200, "text/html", html.c_str());
  } else {
    server.sendHeader("Location", "/");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
  }
}

void handleAddBook() {
  if (is_admin()) {
    String title = server.arg("title");
    String author = server.arg("author");
    String isbn = server.arg("isbn");
    String location = server.arg("location");
    String keywords = server.arg("keywords");
    String synopsis = server.arg("synopsis");

    const char* sql = "INSERT INTO books (title, author, isbn, location, keywords, synopsis) VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, author.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, isbn.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, location.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, keywords.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 6, synopsis.c_str(), -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_DONE) {
  	  int64_t lastId = sqlite3_last_insert_rowid(db);
	  server.sendHeader("Location", "/details?id="+String(lastId));
	  server.sendHeader("Cache-Control", "no-cache");
	  server.send(301);
        } else {
            server.send(500, "text/html", "<h2>Failed to add book!</h2>");
        }
    } else {
        server.send(500, "text/html", "<h2>Failed to prepare statement!</h2>");
    }
    sqlite3_finalize(stmt);
  }
   server.sendHeader("Location", "/");
  server.sendHeader("Cache-Control", "no-cache");
  server.send(301);
}

void renderLogin() {
  IPAddress clientIP = server.client().remoteIP();
  if ( clientIP == allowedHost) {
    String html = renderHeader();
    html += R"(
            <form action="/authenticate" method="POST">
                <label for="username">Username:</label><br>
                <input type="text" id="username" name="username"><br>
                <label for="password">Password:</label><br>
                <input type="password" id="password" name="password"><br><br>
                <input type="submit" value="Login">
            </form>
        </body>
        </html>
    )";
    server.send(200, "text/html", html);
  } else {
    server.sendHeader("Location", "/");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
  }
}

void handleAuthenticate() {
    String username = server.arg("username");
    String password = server.arg("password");

    if (username == "admin" && password == config["admin_password"].c_str()) {  
        String token = generateSessionToken();
        Serial.println(token);
        sessions[token] = username;  
   
        server.sendHeader("Location", "/");
        server.sendHeader("Cache-Control", "no-cache");
        server.sendHeader("Set-Cookie", "session=" + token);
        server.send(301);
        return;
    } else {
        server.send(401, "text/html", "<style> body { background-color: #cccccc; font-family: Arial, Helvetica, Sans-Serif; font-size: large; Color: #000088; }</style><h2>Login failed</h2><p>Invalid credentials.</p><a href=\"/login\">Try Again</a>");
        
    }
}

void handleLogout() {
  String token = server.header("Cookie");
  token = token.substring(token.indexOf("session=") + 8);

  if (is_admin()) {
    sessions.erase(token);
  }
  server.sendHeader("Location", "/");
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Set-Cookie", "session=deleted");
  server.send(301);
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += ( server.method() == HTTP_GET ) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  for ( uint8_t i = 0; i < server.args(); i++ ) {
      message += " " + server.argName ( i ) + ": " + server.arg ( i ) + "\n";
  }

  server.send ( 404, "text/plain", message );
}

void handleViewBooks() {
    int page = server.arg("page").toInt();
    if (page < 1) {
        page = 1; 
    }

    String html = renderHeader();

    const char* sqlCount = "SELECT COUNT(*) FROM books;";
    int totalBooks = 0;

    sqlite3_stmt* stmtCount;
    if (sqlite3_prepare_v2(db, sqlCount, -1, &stmtCount, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmtCount) == SQLITE_ROW) {
            totalBooks = sqlite3_column_int(stmtCount, 0);
        }
    }
    sqlite3_finalize(stmtCount);

    int offset = (page - 1) * RESULTS_PER_PAGE;

    String search = server.arg("search");
    String field = server.arg("field");
    
    String sql = "SELECT * FROM books";
    if (search != "" && field != "") {
        sql += " WHERE " + field + " LIKE ?";
        html += "<h2>Search Results</h2>";
    }
    sql += " LIMIT ? OFFSET ?;";
    sqlite3_stmt* stmt;

    html += "<div>";
    html += "<table border=\"1\">";
    html += "<tr><th>Title</th><th>Author</th><th>ISBN</th><th>Location</th><th>Keywords</th><th>Actions</th></tr>";

    int results = 0;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        int bindIndex = 1;
        
        if (search != "" && field != "") {
            String likeTerm = "%" + search + "%";
            sqlite3_bind_text(stmt, bindIndex++, likeTerm.c_str(), -1, SQLITE_TRANSIENT);
        }

        sqlite3_bind_int(stmt, bindIndex++, RESULTS_PER_PAGE);
        sqlite3_bind_int(stmt, bindIndex, offset);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
          int id = sqlite3_column_int(stmt, 0);
          results += 1;
          html += "<tr>";
          html += "<td>" + String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))) + "</td>";
          html += "<td>" + String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))) + "</td>"; 
          html += "<td>" + String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3))) + "</td>"; 
          html += "<td>" + String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4))) + "</td>"; 
          html += "<td>" + String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5))) + "</td>"; 

          html += "<td><a href=\"/details?id=" + String(id) + "\">Details</a>";
          if (is_admin()) {
            html += "<a href=\"/delete?id=" + String(id) + "\" onclick=\"return confirm('Are you sure you want to delete this book?');\">Delete</a>";
          }
          html += "</td></tr>";
          html += "</tr>";
        }
    } else {
        html += "<tr><td colspan=\"7\">Failed to retrieve books!</td></tr>";
    }

    html += "</table></div><br/>";

    html += "<div>";
    if (page > 1) {
        html += "<a href=\"/?page=" + String(page - 1) + "\">Previous</a> ";
    }
    if (results == RESULTS_PER_PAGE) {
        html += "<a href=\"/?page=" + String(page + 1) + "\">Next</a>";
    }
    html += "</div>";

    html += "</body></html>";

    sqlite3_finalize(stmt);
    
    server.send (200, "text/html", html.c_str());
}

void renderDetails() {
    int id = server.arg("id").toInt();
    String html = renderHeader();

    const char* sql = "SELECT * FROM books WHERE id = ?;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            html += "<p><strong>Title:</strong> " + String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))) + "</p>";
            html += "<p><strong>Author:</strong> " + String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))) + "</p>";
            html += "<p><strong>ISBN:</strong> " + String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3))) + "</p>";
            html += "<p><strong>Location:</strong> " + String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4))) + "</p>";
            html += "<p><strong>Keywords:</strong> " + String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5))) + "</p>";
            html += "<p><strong>Synopsis:</strong> " + String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6))) + "</p>";
            html += "<table><tr><th>Cover</th></tr>";
            html += "<tr><td><img src=\"https://covers.openlibrary.org/b/isbn/"+ String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3))) +"-L.jpg\">";
            html += "</td></tr></table>";
        } else {
            html += "<p>Book not found!</p>";
        }
    }
    sqlite3_finalize(stmt);

    html += "</body></html>";
    server.send(200, "text/html", html);
}

void renderSearch() {
    String html = renderHeader();
    html += "<form action=\"/\" method=\"get\">";
    html += "<label for=\"search\">Search term:</label>";
    html += "<input type=\"text\" name=\"search\" id=\"search\" required>";
    
    html += "<label for=\"field\">Search by:</label>";
    html += "<select name=\"field\" id=\"field\">";
    html += "<option value=\"title\">Title</option>";
    html += "<option value=\"author\">Author</option>";
    html += "<option value=\"isbn\">ISBN</option>";
    html += "<option value=\"location\">Location</option>";
    html += "<option value=\"keywords\">Keywords</option>";
    html += "<option value=\"synopsis\">Synopsis</option>";
    html += "</select>";
    
    html += "<input type=\"submit\" value=\"Search\">";
    html += "</form>";
    html += "</body></html>";

    server.send(200, "text/html", html);
}

void renderEdit() {
  if (is_admin()) {
    int bookID = server.arg("id").toInt();
    
    sqlite3_stmt *stmt;
    String sql = "SELECT title, author, isbn, location, keywords, synopsis FROM books WHERE id = ?";
    
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, bookID);
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            String title = String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
            String author = String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            String isbn = String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
            String location = String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
            String keywords = String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
            String synopsis = String(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
            
            String html = renderHeader();
            html += "<form action=\"/edit\" method=\"POST\">";
            html += "<input type=\"hidden\" name=\"id\" value=\"" + String(bookID) + "\">";
            html += "<label>Title:</label><input type=\"text\" name=\"title\" value=\"" + title + "\"><br>";
            html += "<label>Author:</label><input type=\"text\" name=\"author\" value=\"" + author + "\"><br>";
            html += "<label>ISBN:</label><input type=\"text\" name=\"isbn\" value=\"" + isbn + "\"><br>";
            html += "<label>Location:</label><input type=\"text\" name=\"location\" value=\"" + location + "\"><br>";
            html += "<label>Keywords:</label><input type=\"text\" name=\"keywords\" value=\"" + keywords + "\"><br>";
            html += "<label>Synopsis:</label><textarea name=\"synopsis\">" + synopsis + "</textarea><br>";
            html += "<input type=\"submit\" value=\"Update\">";
            html += "</form>";
            html += "</body></html>";
            
            server.send(200, "text/html", html);
        } else {
            server.send(404, "text/plain", "Book not found");
        }
    } else {
        server.send(500, "text/plain", "Database error");
    }
    
    sqlite3_finalize(stmt);
  } else {
    server.sendHeader("Location", "/");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
  }
}

void handleEditSubmit() {
  if (is_admin()) {
    int bookID = server.arg("id").toInt();
    String title = server.arg("title");
    String author = server.arg("author");
    String isbn = server.arg("isbn");
    String location = server.arg("location");
    String keywords = server.arg("keywords");
    String synopsis = server.arg("synopsis");

    String sql = "UPDATE books SET title = ?, author = ?, isbn = ?, location = ?, keywords = ?, synopsis = ? WHERE id = ?";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, author.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, isbn.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, location.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, keywords.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, synopsis.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 7, bookID);

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            server.sendHeader("Location", "/details?id="+String(bookID));
            server.sendHeader("Cache-Control", "no-cache");
            server.send(301);
        } else {
            server.send(500, "text/plain", "Failed to update book");
        }
    } else {
        server.send(500, "text/plain", "Database error");
    }

    sqlite3_finalize(stmt);
  } else {
    server.sendHeader("Location", "/");
    server.sendHeader("Cache-Control", "no-cache");
    server.send(301);
  }
}

void handleDeleteBook() {
  if (is_admin()) {
    int id = server.arg("id").toInt();
    const char* sql = "DELETE FROM books WHERE id = ?;";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);

        if (sqlite3_step(stmt) == SQLITE_DONE) {
          server.sendHeader("Location", "/");
          server.sendHeader("Cache-Control", "no-cache");
          server.send(301);
          return;
        } else {
            server.send(500, "text/html", "<h2>Failed to delete book!</h2>");
        }
    }
    sqlite3_finalize(stmt);
  } else {
     server.sendHeader("Location", "/");
     server.sendHeader("Cache-Control", "no-cache");
     server.send(301);
     return;
  }
}

void displayImageFiles() {
    String filePath = server.uri(); 
    if (filePath == "/img") {
      String name = server.arg("name");
      filePath = "/img/" + name;
    }
    
    if (SD.exists(filePath)) {
        File file = SD.open(filePath);
        
        String mimeType;
        if (filePath.endsWith(".jpg") || filePath.endsWith(".jpeg")) {
            mimeType = "image/jpeg";
        } else if (filePath.endsWith(".png")) {
            mimeType = "image/png";
        } else if (filePath.endsWith(".gif")) {
            mimeType = "image/gif";
        } else {
            mimeType = "application/octet-stream";
        }
        server.sendHeader("Cache-Control", "max-age=3600"); 
        server.streamFile(file, mimeType);
        file.close();
    } else {
        server.send(404, "text/plain", "404 Not Found");
    }
}

int openDb(const char *filename) {
  //sqlite3_initialize();
  int rc = sqlite3_open(filename, &db);
  if (rc) {
      Serial.printf("Can't open database: %s\n", sqlite3_errmsg(db));
      memset(current_db, '\0', sizeof(current_db));
      return rc;
  } else {
      Serial.printf("Opened database successfully\n");
      strcpy(current_db, filename);
  }
  return rc;
}

void handleBackup() {
  if (is_admin()) {
    sqlite3_close(db);
    const char* filename = strrchr(db_filename, '/');
    File dbFile = SD.open(filename, FILE_READ);
    if (!dbFile) {
	server.send(500, "text/plain", "Failed to retreive backup.");
	return;
    }
  
    time_t now = time(nullptr);
    String backupFilename = "library_db_backup-" + String(now) + ".db";

    server.setContentLength(dbFile.size());
    server.sendHeader("Content-Type", "application/octet-stream");
    server.sendHeader("Content-Disposition", "attachment; filename="+backupFilename);
    server.send(200);
  
    uint8_t buffer[128];
    while (dbFile.available()) {
      size_t bytesRead = dbFile.read(buffer, sizeof(buffer));
      server.client().write(buffer, bytesRead);
    }
  
    dbFile.close();
    
    openDb(db_filename);
  }
}

void setup ( void ) {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.begin(115200);
  server.collectHeaders(headersToCollect,1);
  delay(5000);

  SPI.begin();
  SD.begin();

  readConfig(SD);
  
  Serial.printf("Attempting to connect to %s\n", config["wifi_ssid"].c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(config["wifi_ssid"].c_str(), config["wifi_password"].c_str());
  Serial.println("");

  while ( WiFi.status() != WL_CONNECTED ) {
      delay ( 500 );
      Serial.print ( "." );
  }

  Serial.println ( "" );
  Serial.print ( "Connected to " );
  Serial.println ( config["wifi_ssid"] );
  Serial.print ( "IP address: " );
  Serial.println ( WiFi.localIP() );

  if (!MDNS.begin("library"))  {
    Serial.println("Error setting up MDNS responder!");
  } else {
    Serial.println("OK mDNS");
  }
  
  openDb(db_filename);

  server.on("/", HTTP_GET, handleViewBooks);
  server.on("/login", HTTP_GET, renderLogin);
  server.on("/logout", HTTP_GET, handleLogout);
  server.on("/authenticate", HTTP_POST, handleAuthenticate);
  server.on("/add", HTTP_GET, renderAdd);
  server.on("/add", HTTP_POST, handleAddBook);
  server.on("/details", HTTP_GET, renderDetails);
  server.on("/delete", HTTP_GET, handleDeleteBook);
  server.on("/search", HTTP_GET, renderSearch);
  server.on("/edit", HTTP_GET, renderEdit);
  server.on("/edit", HTTP_POST, handleEditSubmit);
  server.on("/img", HTTP_GET, displayImageFiles);
  server.on("/favicon.ico", HTTP_GET, displayImageFiles);
  server.on("/backup", HTTP_GET, handleBackup);

  server.onNotFound ( handleNotFound );
  server.begin();
  Serial.println ( "HTTP server started" );
}

void loop ( void ) {
  server.handleClient();
}
