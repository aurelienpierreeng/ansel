#   This file is part of the Ansel project.
#   Copyright (C) 2023 Aurélien PIERRE.
#   
#   Ansel is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#   
#   Ansel is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#   
#   You should have received a copy of the GNU General Public License
#   along with Ansel.  If not, see <http://www.gnu.org/licenses/>.










import os
import time
import simplematrixbotlib as botlib
import argparse

parser = argparse.ArgumentParser(
                    prog='Matrix bot',
                    description='Post a message to the Matrix channel',
                    epilog='Done')
parser.add_argument('-m', '--message', help="The message to post")
parser.add_argument('-s', '--server', help="URL of the homeserver")
parser.add_argument('-u', '--user', help="Username of the bot")
parser.add_argument('-t', '--token', help="Access token of the bot")
parser.add_argument('-r', '--room', help="ID of the room, like `!xxxx:matrix.org`")
args = parser.parse_args()

creds = botlib.Creds(homeserver=args.server, username=args.user, access_token=args.token)
bot = botlib.Bot(creds=creds)

@bot.listener.on_startup
async def send_message(joined_room_id: str) -> None:
    if args.room and args.room != joined_room_id:
        return

    message = f"""{args.message}"""

    await bot.api.send_markdown_message(
        room_id=joined_room_id,
        message=message,
        msgtype="m.notice")

    exit()

bot.run()
