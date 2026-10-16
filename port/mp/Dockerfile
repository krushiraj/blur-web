# Lobby / signalling / relay server only (the browser client and demo are static files served by the same process).
# Not built or run as part of the prototype work; kept in sync with `yarn start`.
FROM node:22-slim
WORKDIR /app
ENV NODE_ENV=production PORT=8786 HOST=0.0.0.0 TRUST_PROXY=1
COPY package.json yarn.lock ./
RUN yarn install --production --frozen-lockfile && yarn cache clean
COPY server ./server
COPY client ./client
COPY demo ./demo
COPY test/browser/page.html ./test/browser/page.html
EXPOSE 8786
# UDP_PORT (native harness gateway) is NOT exposed by default
USER node
HEALTHCHECK --interval=15s --timeout=3s CMD node -e "fetch('http://127.0.0.1:'+process.env.PORT+'/health').then(r=>process.exit(r.ok?0:1)).catch(()=>process.exit(1))"
STOPSIGNAL SIGTERM
CMD ["node", "server/index.js"]
