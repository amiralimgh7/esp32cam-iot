import unittest,threading,urllib.request,urllib.error,json
from http.server import ThreadingHTTPServer
from mock_server import Handler

class MockContractTest(unittest.TestCase):
    def test_jpeg_contract_and_rejected_target(self):
        with ThreadingHTTPServer(('127.0.0.1',0),Handler) as server:
            thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            try:
                url=f'http://127.0.0.1:{server.server_port}/upload?device=test&cur=1&target=3&frame=7'
                request=urllib.request.Request(url,data=b'\xff\xd8demo\xff\xd9',headers={'Content-Type':'image/jpeg'})
                with urllib.request.urlopen(request,timeout=2) as response:data=json.load(response)
                self.assertEqual(data['frame'],7);self.assertEqual(data['current_node'],1)
                self.assertEqual(data['command'],'DEMO_ONLY');self.assertEqual(data['current_prob'],0)
                request=urllib.request.Request(url.replace('target=3','target=20'),data=b'\xff\xd8\xff\xd9',headers={'Content-Type':'image/jpeg'})
                with self.assertRaises(urllib.error.HTTPError) as error:urllib.request.urlopen(request,timeout=2)
                self.assertEqual(error.exception.code,400)
            finally:server.shutdown();thread.join()

if __name__=='__main__':unittest.main()
