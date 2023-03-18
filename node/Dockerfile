
FROM node:18.7.0

# VOLUME ["/home/node/app"]
COPY . /home/node
WORKDIR /home/node/app/api
# RUN node --version
RUN npm install typescript -g
RUN npm install

EXPOSE 8888