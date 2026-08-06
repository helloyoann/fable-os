# Dots Services Network — build from the services/ directory:
#   docker build -f deploy/Dockerfile -t dots-services .
FROM node:22-alpine

WORKDIR /app
COPY package.json server.js ./
COPY lib ./lib
COPY public ./public

ENV NODE_ENV=production \
    PORT=8080 \
    DATA_DIR=/data

VOLUME /data
EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=3s \
  CMD wget -qO- http://127.0.0.1:8080/healthz || exit 1

USER node
CMD ["node", "server.js"]
