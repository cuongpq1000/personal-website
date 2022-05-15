# This is the Server Builder 
FROM docker.io/tgagor/centos-stream AS base
LABEL maintainer="godmar@gmail.com"
RUN adduser user -u 1470

FROM alpine AS video-fetch
RUN adduser user -D -u 1470 -h /home/user && \
    apk add curl
USER user
RUN curl -o /tmp/bunny.mp4 https://www.learningcontainer.com/wp-content/uploads/2020/05/sample-mp4-file.mp4

FROM base AS server-build
RUN yum -y install gcc openssl-devel automake libtool git diffutils make procps 
USER user
# This will basically never change
WORKDIR /home/user

COPY --chown=user:user install-dependencies.sh /home/user 
WORKDIR /home/user
RUN sh install-dependencies.sh

# This is the most likely thing to change: 
COPY --chown=user:user src /home/user/src
WORKDIR /home/user/src
RUN make clean
RUN make
# Build the react app
FROM node:lts-alpine AS react-build
RUN adduser user -D -u 1470 -h /home/user
USER user
# React builder (Assuming this will be changed less than the server)
# We will next cache the react dependencies 
COPY --chown=user:user react-app/package.json /home/user/react-app/package.json
COPY --chown=user:user react-app/package-lock.json /home/user/react-app/package-lock.json
WORKDIR /home/user/react-app
RUN npm install
# This is the second least likely thing to change
COPY --chown=user:user react-app/src /home/user/react-app/src
COPY --chown=user:user react-app/public /home/user/react-app/public
RUN npm run build
# Production Build 
FROM base AS final
USER user
WORKDIR /home/user
COPY --from=react-build /home/user/react-app/build/ /home/user/react-app/build/
COPY --from=video-fetch /tmp/bunny.mp4 /home/user/react-app/build/bunny.mp4
COPY --from=server-build /home/user/deps /home/user/deps
COPY --from=server-build /home/user/src/server /home/user/src/server
WORKDIR /home/user/src
CMD ./server -a -p 9999 -R ~/react-app/build 