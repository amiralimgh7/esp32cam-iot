"""Local transport demo. It does not perform image recognition or navigation."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
import argparse
import json

class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        path=urlparse(self.path)
        try:
            query=parse_qs(path.query,strict_parsing=True)
            if path.path!='/upload' or not query.get('device',[''])[0]:raise ValueError('invalid route/device')
            cur=int(query['cur'][0]); target=int(query['target'][0]); frame=int(query['frame'][0])
            length=int(self.headers.get('Content-Length','0'))
            if not 0<=cur<=15 or not 1<=target<=15 or frame<0:raise ValueError('invalid nodes/frame')
            if not 4<=length<=1024*1024:raise ValueError('invalid image size')
            if self.headers.get_content_type()!='image/jpeg':raise ValueError('expected image/jpeg')
            body=self.rfile.read(length)
            if len(body)!=length or not body.startswith(b'\xff\xd8') or not body.endswith(b'\xff\xd9'):raise ValueError('invalid JPEG envelope')
            # Echo the hint; never imply that the mock identified a location.
            result={'ok':True,'frame':frame,'current_node':cur,'current_prob':0.0,
                    'command':'DEMO_ONLY','heading':'unknown','abs_dir':'unknown','next_node':None}
            status=200
        except (KeyError,ValueError) as error:
            result={'ok':False,'err':str(error)};status=400
        encoded=json.dumps(result).encode()
        self.send_response(status);self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(encoded)));self.end_headers();self.wfile.write(encoded)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host',default='127.0.0.1');parser.add_argument('--port',type=int,default=8080)
    args=parser.parse_args()
    with ThreadingHTTPServer((args.host,args.port),Handler) as server:
        print(f'Mock upload endpoint: http://{args.host}:{args.port}/upload')
        server.serve_forever()
