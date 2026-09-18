FROM node:20-slim

LABEL maintainer="30hours <nathan@30hours.dev>"
LABEL org.opencontainers.image.source https://github.com/30hours/blah2

# Create app directory
WORKDIR /usr/src/app

# Install app dependencies
# A wildcard is used to ensure both package.json AND package-lock.json are copied
# where available (npm@5+)
COPY api/package*.json ./

# devDependencies only generate openapi.json, so they stay out of the image
RUN npm ci --omit=dev

# Bundle app source
COPY api/ .

EXPOSE 8080
CMD [ "node", "server.js" ]
