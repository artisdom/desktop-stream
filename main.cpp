#include "server_ws.hpp"
#include "server_http.hpp"

#include "process.hpp"

#include <boost/filesystem.hpp>

#include <fstream>
#include <unordered_set>
#include <future>

using namespace std;

const string ip_address="localhost";
const boost::filesystem::path screenshot_directory(".");
const string screenshot_filename="screenshot_resized.png";
//ImageMagick commands:
#if defined(__APPLE__)
const string screenshot_command("screencapture -x screenshot.png && convert screenshot.png -strip -resize 75% "+screenshot_filename);
#else
const string screenshot_command("import -window root -silent -strip -resize 75% "+screenshot_filename);
#endif
const size_t delay=100; //delay in milliseconds

typedef SimpleWeb::SocketServer<SimpleWeb::WS> WsServer;
typedef SimpleWeb::Server<SimpleWeb::HTTP> HttpServer;

const string html=R"RAW(<html>
<head>
<meta http-equiv='Content-Type' content='text/html; charset=utf-8' />
<title>Desktop Stream</title>
</head>
<body>
<div id='status'></div>
<img id='image'/>
<script>
var ws;
window.onload=function(){
  ws=new WebSocket('ws://)RAW"+ip_address+R"RAW(:8081/desktop');
  ws.onmessage=function(evt){
    var blob = new Blob([evt.data], {type: 'application/octet-binary'});

    var image = document.getElementById('image');

    var reader = new FileReader();
    reader.onload = function(e) {
      image.src = e.target.result;
    };
    reader.readAsDataURL(blob);
  };
  ws.onclose=function(evt){
    document.getElementById('status').innerHTML = '<b>Connection closed, reload page to reconnect.</b>';
  };
  ws.onerror=function(evt){
    document.getElementById('status').innerHTML = '<b>Connection error, reload page to reconnect.</b>';
  };
};
</script>
</body>
</html>)RAW";

int main() {
    //WebSocket (WS)-server at port 8080 using 4 threads
    HttpServer http_server(8080, 1);
    WsServer ws_server(8081, 1);
    
    auto& desktop_endpoint=ws_server.endpoint["^/desktop/?$"];
    
    vector<char> image_buffer;
    vector<char> last_image_buffer;
    
    unordered_set<shared_ptr<WsServer::Connection> > connections_receiving, connections_skipped;
    
    if(!boost::filesystem::exists(screenshot_directory)) {
        cerr << screenshot_directory << " does not exist, please create it or change the screenshot directory path" << endl;
        return 1;
    }
    
    boost::filesystem::path screenshot_path=screenshot_directory/screenshot_filename;
    if(!boost::filesystem::exists(screenshot_path)) {
        Process process(screenshot_command, screenshot_directory.string(), nullptr, [](const char *bytes, size_t n) {
            cerr << string(bytes, n);
        });
        if(process.get_exit_status()!=0)
            return 1;
    }
    
    ifstream ifs;
    ifs.open(screenshot_path.string(), ifstream::in | ios::binary);
    ifs.seekg(0, ios::end);
    size_t length=ifs.tellg();
    ifs.seekg(0, ios::beg);
    
    image_buffer.resize(length);
    ifs.read(&image_buffer[0], length);
    
    ifs.close();
    
    thread update_image_thread([&](){
        while(true) {
            Process process(screenshot_command, screenshot_directory.string(), nullptr, [](const char *bytes, size_t n) {
                cerr << string(bytes, n);
            });
            if(process.get_exit_status()==0) {
                promise<void> promise;
                ws_server.io_service->post([&] {
                    ifs.open(screenshot_path.string(), ifstream::in | ios::binary);
                    ifs.seekg(0, ios::end);
                    size_t length=ifs.tellg();
                    ifs.seekg(0, ios::beg);
                    
                    image_buffer.resize(length);
                    ifs.read(&image_buffer[0], length);
                    bool equal_buffer=(image_buffer==last_image_buffer);
                    if(!equal_buffer)
                        last_image_buffer=image_buffer;
                    
                    ifs.close();
                    
                    unordered_set<shared_ptr<WsServer::Connection> > connections;
                    if(equal_buffer)
                        connections=connections_skipped;
                    else
                        connections=desktop_endpoint.get_connections();
                    for(auto a_connection: connections) {
                        bool skip_connection=false;
                        if(connections_receiving.count(a_connection)>0) {
                            skip_connection=true;
                            connections_skipped.emplace(a_connection);
                        }
                        
                        if(!skip_connection) {
                            connections_skipped.erase(a_connection);
                            
                            auto send_stream=make_shared<WsServer::SendStream>();
                            
                            send_stream->write(image_buffer.data(), image_buffer.size());
                            
                            connections_receiving.emplace(a_connection);
                            ws_server.send(a_connection, send_stream, [&connections_receiving, a_connection](const boost::system::error_code &ec) {
                                connections_receiving.erase(a_connection);
                            }, 130);
                        }
                    }
                    promise.set_value();
                });
                promise.get_future().wait();
            }
            this_thread::sleep_for(chrono::milliseconds(delay));
        }
    });
    
    desktop_endpoint.onopen=[&](shared_ptr<WsServer::Connection> connection) {
        auto send_stream=make_shared<WsServer::SendStream>();
        
        send_stream->write(image_buffer.data(), image_buffer.size());
        
        connections_receiving.emplace(connection);
        ws_server.send(connection, send_stream, [&connections_receiving, connection](const boost::system::error_code &ec) {
            connections_receiving.erase(connection);
        }, 130);
    };
    
    desktop_endpoint.onclose=[&](shared_ptr<WsServer::Connection> connection, int status, const string& reason) {
        connections_receiving.erase(connection);
        connections_skipped.erase(connection);
    };
    
    desktop_endpoint.onerror=[&](shared_ptr<WsServer::Connection> connection, const boost::system::error_code& ec) {
        connections_receiving.erase(connection);
        
        connections_skipped.erase(connection);
        
        cerr << "Error: " << ec << ", error message: " << ec.message() << endl;
    };
    
    http_server.default_resource["GET"]=[](shared_ptr<HttpServer::Response> response, shared_ptr<HttpServer::Request> request) {
        *response << "HTTP/1.1 200 OK\r\nContent-Length: " << html.size() << "\r\n\r\n" << html;
    };
    
    thread server_thread([&http_server](){
        //Start server
        http_server.start();
    });
    
    ws_server.start();
    server_thread.join();
    update_image_thread.join();
    
    return 0;
}
