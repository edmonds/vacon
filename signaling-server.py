#!/usr/bin/env python3
#
# Vacon signaling server, based on:
#
# Python signaling server example for libdatachannel
# Copyright (c) 2020 Paul-Louis Ageneau
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

import sys
import ssl
import json
import asyncio
import logging
import websockets
import traceback

logger = logging.getLogger('websockets')
logger.setLevel(logging.INFO)
logger.addHandler(logging.StreamHandler(sys.stdout))

sessions = {}

async def handle_websocket(websocket, path):
    session_id = None
    try:
        if not path.startswith('/v1/ooo/'):
            return
        session_id = path.partition('/v1/ooo/')[-1]

        print('Client connected for session {}'.format(session_id))

        if not session_id in sessions:
            sessions[session_id] = { "peers": [ websocket ], "started": False }
        else:
            sessions[session_id]['peers'].append(websocket)

        while True:
            session = sessions.get(session_id)
            if len(session['peers']) >= 2 and session['started'] == False:
                session['started'] = True
                data = json.dumps({"type": "start_session"})
                first_peer = session['peers'][0]
                print('Session {}: {} sending {}'.format(session_id, first_peer.remote_address, data))
                await first_peer.send(data)
            data = await websocket.recv()
            print('Session {}: {} received {}'.format(session_id, websocket.remote_address, data))

            for peer in session['peers']:
                if peer != websocket:
                    print('Session {}: {} sending {}'.format(session_id, peer.remote_address, data))
                    await peer.send(data)

    except websockets.exceptions.ConnectionClosedError:
        print('Peer closed connection')
    except Exception as e:
        traceback.print_exception(e)

    finally:
        if session_id:
            if session_id in sessions:
                if websocket in sessions[session_id]['peers']:
                    print('Removing peer {} from session {}'.format(websocket.remote_address, session_id))
                    sessions[session_id]['peers'].remove(websocket)
                if len(sessions[session_id]['peers']) == 0:
                    print('Deleting session {}, last peer disconnected'.format(session_id))
                    del sessions[session_id]

async def main():
    # Usage: ./server.py [[host:]port] [SSL certificate file]
    endpoint_or_port = sys.argv[1] if len(sys.argv) > 1 else "8000"
    ssl_cert = sys.argv[2] if len(sys.argv) > 2 else None

    endpoint = endpoint_or_port if ':' in endpoint_or_port else "127.0.0.1:" + endpoint_or_port

    if ssl_cert:
        ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_context.load_cert_chain(ssl_cert)
    else:
        ssl_context = None

    print('Listening on {}'.format(endpoint))
    host, port = endpoint.rsplit(':', 1)

    server = await websockets.serve(handle_websocket, host, int(port), ssl=ssl_context)
    await server.wait_closed()

if __name__ == '__main__':
    asyncio.run(main())
